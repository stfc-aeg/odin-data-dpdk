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
        camera_controller_(NULL),
        camera_property_update_(false),
        in_capture_(false),
        captured_frames_(0),
        dropped_frames_(0),
        captured_frames_hz_(0),
        last_frame_(0),
        idle_loops_(0)

    {

        config_.resolve(dpdkWorkCoreReferences.core_config, dpdkWorkCoreReferences.config_key);

        LOG4CXX_INFO(logger_, "CameraCaptureCore " << proc_idx_ << " Created with config:"
            << " | core_name: " << config_.core_name
            << " | num_downstream_cores: " << config_.num_downstream_cores
        );

        // Create downstream rings, or look them up if already created by a sibling core
        for (int ring_idx = 0; ring_idx < config_.num_downstream_cores; ring_idx++)
        {
            std::string downstream_ring_name = ring_name_str(config_.config_key, socket_id_, ring_idx);
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
        std::string clear_frames_ring_name = ring_name_clear_frames(socket_id_, config_.stream_id);
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
            else
            {
                // Populate the ring with hugepages memory locations to the SMB
                for (int element = 0; element < shared_buf_->get_num_buffers(); element++)
                {

                    rte_ring_enqueue(clear_frames_ring_, shared_buf_->get_buffer_address(element));
                }
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

        // Per-second rate accounting, matching the pattern used by the other worker cores
        uint64_t frames_this_second = 0;
        uint64_t idle_loops = 0;
        uint64_t last = rte_get_tsc_cycles();
        uint64_t cycles_per_sec = rte_get_tsc_hz();

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
            uint64_t now = rte_get_tsc_cycles();
            if (unlikely((now - last) >= cycles_per_sec))
            {
                // Publish the monitoring counters once per second and reset the accumulators
                captured_frames_hz_ = frames_this_second;
                idle_loops_ = idle_loops;

                frames_this_second = 0;
                idle_loops = 0;
                last = now;
            }

            auto* cam = camera_controller_->camera_.get();
            bool capturing = cam->get_state_name() == "capturing";
            bool in_window = cam->camera_config_->num_frames_ == 0 ||
                             cam->camera_status_->frame_number_ <= cam->camera_config_->num_frames_;

            in_capture_ = capturing && in_window;

            if (capturing && in_window)
            {
                frame_src = camera_controller_->get_frame();

                if (frame_src != nullptr)
                {
                    if (unlikely(rte_ring_dequeue(clear_frames_ring_, (void **)&current_super_frame_buffer_)) != 0)
                    {
                        dropped_frames_++;
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

                        captured_frames_++;
                        frames_this_second++;
                    }

                    last_frame_ = cam->camera_status_->frame_number_;

                    if (cam->camera_status_->frame_number_ % 1000 == 0)
                    {
                        LOG4CXX_INFO(logger_, "Core " << lcore_id_ << " Captured frame " << cam->camera_status_->frame_number_);
                    }

                    cam->camera_status_->frame_number_++;
                }
                else
                {
                    // Camera had no frame ready this iteration
                    idle_loops++;
                }
            }
            else
            {
                // Not capturing, or the configured frame target has been reached
                idle_loops++;
            }
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
        // Scoped by config_key rather than the class name so that capture cores in different
        // streams report under distinct keys instead of overwriting each other.
        std::string core_key = config_.config_key.empty() ? config_.core_name : config_.config_key;
        std::string status_path = path + "/" + core_key + "_" + std::to_string(proc_idx_) + "/";
        std::string ring_status = status_path + "downstream_rings/";
        std::string camera_status = status_path + "camera/";

        status.set_param(status_path + "stream_id", config_.stream_id);
        status.set_param(status_path + "lcore_id", (int)lcore_id_);
        status.set_param(status_path + "running", run_lcore_);
        status.set_param(status_path + "in_capture", in_capture_);

        status.set_param(status_path + "frames_captured", captured_frames_);
        status.set_param(status_path + "frames_captured_per_second", captured_frames_hz_);
        status.set_param(status_path + "frames_dropped", dropped_frames_);
        status.set_param(status_path + "last_frame_number", last_frame_);
        status.set_param(status_path + "idle_loops", idle_loops_);

        // Camera-side view of progress, so a stalled capture can be told apart from a stalled
        // camera. The controller is only available once run() has started.
        // Read through to the camera the same way run() does. CameraController declares
        // camera_state_name()/get_frame_number()/get_frame_target() but never defines them, so
        // using those accessors here would not link.
        if (camera_controller_ != NULL && camera_controller_->camera_ != NULL)
        {
            auto* cam = camera_controller_->camera_.get();
            status.set_param(camera_status + "state", cam->get_state_name());
            status.set_param(camera_status + "frame_number", (uint64_t)cam->camera_status_->frame_number_);
            status.set_param(camera_status + "frame_target", (uint64_t)cam->camera_config_->num_frames_);
        }

        // Free-buffer availability: a clear_frames count pinned at zero explains frames_dropped
        if (clear_frames_ring_ != NULL)
        {
            status.set_param(ring_status + ring_name_clear_frames(socket_id_, config_.stream_id) + "_count", rte_ring_count(clear_frames_ring_));
            status.set_param(ring_status + ring_name_clear_frames(socket_id_, config_.stream_id) + "_size", rte_ring_get_size(clear_frames_ring_));
        }

        // Downstream ring occupancy, showing whether consumers are keeping up
        for (std::size_t ring_idx = 0; ring_idx < downstream_rings_.size(); ring_idx++)
        {
            std::string ring_name = ring_name_str(config_.config_key, socket_id_, ring_idx);
            status.set_param(ring_status + ring_name + "_count", rte_ring_count(downstream_rings_[ring_idx]));
            status.set_param(ring_status + ring_name + "_size", rte_ring_get_size(downstream_rings_[ring_idx]));
        }
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