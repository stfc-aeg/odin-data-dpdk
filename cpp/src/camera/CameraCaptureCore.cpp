#include "camera/CameraCaptureCore.h"

#include <rte_malloc.h>
#include <stdexcept>
#include "DpdkUtils.h"

namespace FrameProcessor
{
    CameraCaptureCore::CameraCaptureCore(
        int proc_idx, int socket_id, DpdkWorkCoreReferences dpdkWorkCoreReferences
    ) :
        DpdkWorkerCore(socket_id),
        proc_idx_(proc_idx),
        decoder_(dpdkWorkCoreReferences.decoder),
        shared_buf_(dpdkWorkCoreReferences.shared_buf),
        logger_(Logger::getLogger("FP.CameraCaptureCore")),
        camera_controller_(NULL)
        
    {

        config_.resolve(dpdkWorkCoreReferences.core_config);

        LOG4CXX_INFO(logger_, "CameraCaptureCore " << proc_idx_ << " Created with config:"
            << " | core_name: " << config_.core_name
            << " | num_downstream_cores: " << config_.num_downstream_cores
        );

        // Create downstream rings, or look them up if already created by a sibling core
        for (int ring_idx = 0; ring_idx < config_.num_downstream_cores; ring_idx++)
        {
            std::string downstream_ring_name = ring_name_str(config_.core_name, socket_id_, ring_idx);
            struct rte_ring* downstream_ring = rte_ring_lookup(downstream_ring_name.c_str());
            if (downstream_ring == NULL)
            {
                unsigned int downstream_ring_size = nearest_power_two(shared_buf_->get_num_buffers());
                LOG4CXX_INFO(logger_, "Creating ring name "
                    << downstream_ring_name << " of size " << downstream_ring_size
                );
                downstream_ring = rte_ring_create(
                    downstream_ring_name.c_str(), downstream_ring_size, socket_id_, 0
                );
                if (downstream_ring == NULL)
                {
                    throw std::runtime_error("Failed to create downstream ring " + downstream_ring_name
                        + ": " + rte_strerror(rte_errno));
                }
            }
            downstream_rings_.push_back(downstream_ring);
        }

        // Create the clear_frames ring (shared buffer pool), or look it up if already created
        std::string clear_frames_ring_name = ring_name_clear_frames(socket_id_);
        clear_frames_ring_ = rte_ring_lookup(clear_frames_ring_name.c_str());
        if (clear_frames_ring_ == NULL)
        {
            unsigned int clear_frames_ring_size = nearest_power_two(shared_buf_->get_num_buffers());
            clear_frames_ring_ = rte_ring_create(
                clear_frames_ring_name.c_str(), clear_frames_ring_size, socket_id_, 0
            );
            if (clear_frames_ring_ == NULL)
            {
                throw std::runtime_error("Failed to create clear_frames ring " + clear_frames_ring_name
                    + ": " + rte_strerror(rte_errno));
            }
            for (int element = 0; element < shared_buf_->get_num_buffers(); element++)
            {
                rte_ring_enqueue(clear_frames_ring_, shared_buf_->get_buffer_address(element));
            }
        }
    }

    CameraCaptureCore::~CameraCaptureCore()
    {
        LOG4CXX_DEBUG_LEVEL(2, logger_, "CameraCaptureCore destructor");

        // Stop the core polling loop so the run method terminates
        stop();
    }

    bool CameraCaptureCore::run(unsigned int lcore_id)
    {

        lcore_id_ = lcore_id;
        run_lcore_ = true;

        LOG4CXX_INFO(logger_, "Core " << lcore_id_ << " starting up");

        struct SuperFrameHeader *current_super_frame_buffer_;
        char* frame_src;
        uint64_t frame_size = decoder_->get_frame_bit_depth() * decoder_->get_frame_x_resolution() * decoder_->get_frame_y_resolution();
        uint64_t dropped_frames = 0;

        in_capture_ = false;

        camera_controller_ = CameraController::Instance("CameraController_");
        if (camera_controller_ == NULL)
        {
            LOG4CXX_ERROR(logger_, "Unable to get controller instance, capture core terminating");
            return false;
        }
        camera_controller_->register_capture_core(this);

        while (likely(run_lcore_))
        {
            auto* cam = camera_controller_->camera_.get();
            bool capturing = cam->get_state_name() == "capturing";
            bool in_window = cam->camera_config_->num_frames_ == 0 ||
                             cam->camera_status_->frame_number_ <= cam->camera_config_->num_frames_;

            if (capturing && in_window)
            {
                frame_src = camera_controller_->get_frame();

                if (frame_src != nullptr)
                {
                    if (unlikely(rte_ring_dequeue(clear_frames_ring_, (void **)&current_super_frame_buffer_)) != 0)
                    {
                        dropped_frames++;
                        LOG4CXX_DEBUG(logger_, "Dropping frame " << cam->camera_status_->frame_number_ << " — no buffer available");
                    }
                    else
                    {
                        memset(current_super_frame_buffer_, 0, decoder_->get_frame_buffer_size());
                        decoder_->set_super_frame_number(current_super_frame_buffer_, cam->camera_status_->frame_number_);
                        decoder_->set_super_frame_start_time(current_super_frame_buffer_, rte_get_tsc_cycles());

                        rte_memcpy(decoder_->get_image_data_start(current_super_frame_buffer_), frame_src, frame_size);
                        decoder_->set_super_frame_image_size(current_super_frame_buffer_, frame_size);

                        rte_ring_enqueue(
                            downstream_rings_[
                                decoder_->get_super_frame_number(current_super_frame_buffer_) %
                                config_.num_downstream_cores
                            ], current_super_frame_buffer_
                        );
                    }

                    if (cam->camera_status_->frame_number_ % 1000 == 0)
                    {
                        LOG4CXX_INFO(logger_, "Core " << lcore_id_ << " Captured frame " << cam->camera_status_->frame_number_);
                    }

                    cam->camera_status_->frame_number_++;
                }
            }
            in_capture_ = false;
        }
        return true;
    }

    void CameraCaptureCore::stop(void)
    {
        if (run_lcore_)
        {
            LOG4CXX_INFO(logger_, "Core " << lcore_id_ << " stopping");

            run_lcore_ = false;
        }
        else
        {
            LOG4CXX_DEBUG_LEVEL(2, logger_, "Core " << lcore_id_ << " already stopped");
        }
    }

    void CameraCaptureCore::status(OdinData::IpcMessage& status, const std::string& path)
    {
        std::string status_path = path + "/CameraCaptureCore_" + std::to_string(proc_idx_) + "/";
    }

    bool CameraCaptureCore::connect(void)
    {
        LOG4CXX_INFO(logger_, "Core " << proc_idx_ << " connecting...");
        return true;
    }

    void CameraCaptureCore::configure(OdinData::IpcMessage& config)
    {
        LOG4CXX_INFO(logger_, config_.core_name << " : " << proc_idx_ << " Got update config.");
    }

    void* CameraCaptureCore::pop_empty_buffer(void)
    {
        void* buffer;

        if (unlikely(rte_ring_dequeue(clear_frames_ring_, (void **)&buffer)) != 0)
        {
            return nullptr;
        }
        return buffer;
    }

    void CameraCaptureCore::push_empty_buffer(void* buffer)
    {
        rte_ring_enqueue(clear_frames_ring_, buffer);
    }

    DPDKREGISTER(DpdkWorkerCore, CameraCaptureCore, "CameraCaptureCore");
}