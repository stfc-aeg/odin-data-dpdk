
#include "camera/CameraCaptureCore.h"

#include <iostream>
#include <unordered_map>
#include <string>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_udp.h>
#include <rte_malloc.h>
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

        LOG4CXX_INFO(logger_, "Core CameraCaptureCore" << proc_idx_ << " creation!");

        // Resolve configuration parameters for this core from the config object passed as an
        // argument, and the current port ID
        config_.resolve(dpdkWorkCoreReferences.core_config);

        LOG4CXX_INFO(logger_, "Core CameraCaptureCore " << proc_idx_ << " config resolved!");

        // Check if the downstream ring have already been created by another processing core,
        // otherwise create it with the ring size rounded up to the next power of two
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
                    LOG4CXX_ERROR(logger_, "Error creating downstream ring " << downstream_ring_name
                        << " : " << rte_strerror(rte_errno)
                    );
                    // TODO - this is fatal and should raise an exception
                }
            }
            else
            {
                LOG4CXX_DEBUG_LEVEL(2, logger_, "downstream ring with name "
                    << downstream_ring_name << " has already been created"
                );
            }
            if (downstream_ring)
            {
                downstream_rings_.push_back(downstream_ring);
            }

        }

        // Check if the clear_frames ring has already been created by another procsssing core,
        // otherwise create it with the ring size rounded up to the next power of two
        std::string clear_frames_ring_name = ring_name_clear_frames(socket_id_);
        clear_frames_ring_ = rte_ring_lookup(clear_frames_ring_name.c_str());
        if (clear_frames_ring_ == NULL)
        {
            unsigned int clear_frames_ring_size = nearest_power_two(shared_buf_->get_num_buffers());
            LOG4CXX_DEBUG_LEVEL(2, logger_, "Creating frame processed ring name "
                << clear_frames_ring_name << " of size " << clear_frames_ring_size
            );
            clear_frames_ring_ = rte_ring_create(
                clear_frames_ring_name.c_str(), clear_frames_ring_size, socket_id_, 0
            );
            if (clear_frames_ring_ == NULL)
            {
                LOG4CXX_ERROR(logger_, "Error creating frame processed ring " << clear_frames_ring_name
                    << " : " << rte_strerror(rte_errno)
                );
                // TODO - this is fatal and should raise an exception
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

        // Declares

        struct RawFrameHeader *current_frame_header_;
        struct SuperFrameHeader *current_super_frame_buffer_;

        char* frame_src;

        uint64_t frame_size = decoder_->get_frame_bit_depth() * decoder_->get_frame_x_resolution() * decoder_->get_frame_y_resolution();


        uint64_t dropped_frames_ = 0;

        LOG4CXX_INFO(logger_, "Core " << lcore_id_ << " Connecting to camera\n");

        bool passed = false;
        in_capture_ = false;


        // Get the controller instance
        camera_controller_ = CameraController::Instance("CameraController_");
        if (camera_controller_ == NULL)
        {
            LOG4CXX_ERROR(logger_, "Unable to get controller instance, capture core terminating");
            return false;
        }

        // Register ourself with the controller
        camera_controller_->register_capture_core(this);

        

        while (likely(run_lcore_))
        {
            // Check camera is recording

            LOG4CXX_DEBUG(logger_, "Frame Grab check: \n Camera Status: " << camera_controller_->camera_->camera_status_->camera_status_ << " Num frames: " << camera_controller_->camera_->camera_config_->num_frames_ << " Frame Number: " << camera_controller_->camera_->camera_status_->frame_number_);
            
            if(camera_controller_->camera_->camera_status_->camera_status_ == "capturing" && 
                // Check if running forever
                (camera_controller_->camera_->camera_config_->num_frames_ == 0 || 
                // Check frame is in acquisition
                camera_controller_->camera_->camera_status_->frame_number_ <= camera_controller_->camera_->camera_config_->num_frames_))
            {
                //LOG4CXX_INFO(logger_, "Core " << lcore_id_ << " Loop");
                // This frame should be captured
                LOG4CXX_DEBUG(logger_, "Attempt to get frame:");
                frame_src = camera_controller_->get_frame();
                

                // Check if a valid frame pointer was returned
                if(frame_src != nullptr)
                {
                    // Get a hugepages memory location for this frame
                    if (unlikely(rte_ring_dequeue(
                        clear_frames_ring_, (void **) &current_super_frame_buffer_
                    )) != 0)
                    {
                        // Memory location cannot be found start dropping this frame
                        dropped_frames_++;
                        LOG4CXX_DEBUG(logger_,
                                "dropping frame: " << camera_controller_->camera_->camera_status_->frame_number_);
                    }
                    else
                    {
                        //std::cout << "Got new frame: " << camera_controller_->camera_.camera_status_->frame_number_ << std::endl;
                        // Zero out the frame header to clear old data
                        memset(current_super_frame_buffer_, 0, decoder_->get_frame_buffer_size());

                        // Set the frame number and start time in the header
                        decoder_->set_super_frame_number(current_super_frame_buffer_, camera_controller_->camera_->camera_status_->frame_number_);
                        decoder_->set_super_frame_start_time(
                            current_super_frame_buffer_, rte_get_tsc_cycles()
                        );

                        current_frame_header_ = decoder_->get_frame_header(current_super_frame_buffer_, 0);
                        

                        // Copy the frame data to the frame struct
                        rte_memcpy(decoder_->get_image_data_start(current_super_frame_buffer_), frame_src, frame_size);

                        decoder_->set_super_frame_image_size(current_super_frame_buffer_, frame_size);


                        // Enqeue the frame to one of the downstream cores
                        rte_ring_enqueue(
                                    downstream_rings_[
                                        decoder_->get_super_frame_number(current_super_frame_buffer_) % 
                                        config_.num_downstream_cores
                                    ], current_super_frame_buffer_
                                );
                    }


                    // Debug print per 1000 frames
                    // This can be removed
                    if(camera_controller_->camera_->camera_status_->frame_number_ % 1000 == 0)
                    {
                        LOG4CXX_INFO(logger_, "Core " << lcore_id_ << " Captured framed " << camera_controller_->camera_->camera_status_->frame_number_);
                    }


                    camera_controller_->camera_->camera_status_->frame_number_++;


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
        LOG4CXX_DEBUG(logger_, "Status requested for Camera_Capture_Core_" << proc_idx_
            << " from the DPDK plugin");

        std::string status_path = path + "/CameraCaptureCore_" + std::to_string(proc_idx_) + "/";
    }

    bool CameraCaptureCore::connect(void)
    {


        LOG4CXX_INFO(logger_, "Core " << proc_idx_ << " connecting...");
 
        return true;
    }


    void CameraCaptureCore::configure(OdinData::IpcMessage& config)
    {
        // Update the config based from the passed IPCmessage

        LOG4CXX_INFO(logger_, config_.core_name << " : " << proc_idx_ << " Got update config: \n" << config);
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