#include "FrameWrapperCore.h"
#include "DpdkUtils.h"
#include <blosc.h>
#include "DpdkSharedBufferFrame.h"
#include </usr/lib/x86_64-linux-gnu/hdf5/serial/include/hdf5.h>

namespace FrameProcessor
{
    FrameWrapperCore::FrameWrapperCore(
        int fb_idx, int socket_id, DpdkWorkCoreReferences &dpdkWorkCoreReferences
    ) :
        DpdkWorkerCore(socket_id),
        logger_(Logger::getLogger("FP.FrameWrapperCore")),
        proc_idx_(fb_idx),
        decoder_(dpdkWorkCoreReferences.decoder),
        frame_callback_(dpdkWorkCoreReferences.frame_callback),
        frames_wrapped_(0),
        frames_wrapped_hz_(0),
        idle_loops_(0),
        avg_us_spent_wrapping_(0)
    {

        // Get the configuration container for this worker
        config_.resolve(dpdkWorkCoreReferences.core_config);

        LOG4CXX_INFO(logger_, "FP.FrameWrapperCore " << proc_idx_ << " Created with config:"
            << " | core_name" << config_.core_name
            << " | num_cores: " << config_.num_cores
            << " | connect: " << config_.connect
            << " | upstream_core: " << config_.upstream_core
            << " | num_downsteam_cores: " << config_.num_downstream_cores
        );

    }

    FrameWrapperCore::~FrameWrapperCore(void)
    {
        LOG4CXX_DEBUG_LEVEL(2, logger_, "FrameWrapperCore destructor");
        stop();
    }

    bool FrameWrapperCore::run(unsigned int lcore_id)
    {

        lcore_id_ = lcore_id;
        run_lcore_ = true;

        LOG4CXX_INFO(logger_, "Core " << lcore_id_ << " starting up");

        // Blosc compression settings
        const char * p_compressor_name;
        blosc_compcode_to_compname(1, &p_compressor_name);

        // Frame variables
        struct SuperFrameHeader *current_super_frame_buffer_;
        dimensions_t dims(2);

        // Specific frame variables from decoder
        dims[0] = decoder_->get_frame_x_resolution();
        dims[1] = decoder_->get_frame_y_resolution();
        std::size_t frame_size = decoder_->get_frame_data_size() * decoder_->get_frame_outer_chunk_size();
            //dims[0] * dims[1] * get_size_from_enum(decoder_->get_frame_bit_depth());
        std::size_t frame_header_size = decoder_->get_frame_header_size();

        // Status reporting variables
        uint64_t frames_per_second = 0;
        uint64_t last = rte_get_tsc_cycles();
        uint64_t cycles_per_sec = rte_get_tsc_hz();
        uint64_t start_compressing = 1;
        uint64_t average_wrapping_cycles = 1;
        uint64_t last_Frame = -1;
        uint64_t frames_wrapped_ = 0;
        uint64_t data_pointer_offset = (frame_header_size * decoder_->get_frame_outer_chunk_size()) + decoder_->get_super_frame_header_size();

        //While loop to continuously dequeue frame objects
        while (likely(run_lcore_))
        {
            uint64_t now = rte_get_tsc_cycles();
            if (unlikely((now - last) >= (cycles_per_sec)))
            {
                // Update any monitoring variables every second
                frames_wrapped_hz_ = frames_per_second;
                avg_us_spent_wrapping_ = (average_wrapping_cycles * 1000000 )/ cycles_per_sec;

                // Reset any counters
                frames_per_second = 0;
                idle_loops_ = 0;
                average_wrapping_cycles = 0;

                last = now;
            }
            // Attempt to dequeue a new frame object
            if (rte_ring_dequeue(upstream_ring_, (void**) &current_super_frame_buffer_) < 0)
            {
                // No frame was dequeued, try again
                idle_loops_++;
                continue;
            }
            else
            {
                uint64_t frame_number = decoder_->get_super_frame_number(current_super_frame_buffer_);
                last_Frame = frame_number;

                decoder_->set_super_frame_image_size(current_super_frame_buffer_, frame_size);

                // Create new frame metadata object
                FrameMetaData frame_meta;
                frame_meta.set_dataset_name(config_.dataset_name_);
                frame_meta.set_frame_number(frame_number);
                frame_meta.set_dimensions(dims);
                frame_meta.set_data_type(decoder_->get_frame_bit_depth());

                // Get the image size, with this we can work out if the frame has been compressed
                uint64_t image_size = decoder_->get_super_frame_image_size(current_super_frame_buffer_);

                if (frame_size != image_size)
                {
                    frame_meta.set_compression_type(blosc);
                }
                else
                {
                    frame_meta.set_compression_type(no_compression);
                }

                // Create the shared boost pointer to allow the plugin chain to access huge pages
                boost::shared_ptr<Frame> complete_frame =
                        boost::shared_ptr<Frame>(new DpdkSharedBufferFrame(
                                                    frame_meta, current_super_frame_buffer_,
                                                    decoder_->get_frame_buffer_size(),
                                                    clear_frames_ring_, data_pointer_offset));

                complete_frame->set_image_size(decoder_->get_super_frame_image_size(current_super_frame_buffer_));
                complete_frame->set_outer_chunk_size(decoder_->get_frame_outer_chunk_size());
                frame_callback_(complete_frame);

                // Update monitoring variables now that the Frame has been pushed
                average_wrapping_cycles = 
                    (average_wrapping_cycles + (rte_get_tsc_cycles() - start_compressing)) / 2;

                frames_per_second++;
                frames_wrapped_++;

                // if(frame_number % 1000 == 0)
                // {
                //     LOG4CXX_INFO(logger_, "Wrapped frame: " << frame_number * decoder_->get_frame_outer_chunk_size());
                // }

                //LOG4CXX_INFO(logger_, "Wrapped frame: " << frame_number);
            }
        }

        LOG4CXX_INFO(logger_, "Core " << lcore_id_ << " completed");

        return true;
    }

    void FrameWrapperCore::stop(void)
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

    void FrameWrapperCore::status(OdinData::IpcMessage& status, const std::string& path)
    {
        LOG4CXX_DEBUG(logger_, "Status requested for FrameWrapperCore_" << proc_idx_
            << " from the DPDK plugin");

        std::string status_path = path + "/FrameWrapperCore_" + std::to_string(proc_idx_) + "/";

        status.set_param(status_path + "frames_wrapped", frames_wrapped_);

        status.set_param(status_path + "frames_wrapped_hz", frames_wrapped_hz_);

        status.set_param(status_path + "idle_loops", idle_loops_);

        status.set_param(status_path + "frames_wrapped_us_compressing", avg_us_spent_wrapping_);
    }

    bool FrameWrapperCore::connect(void)
    {

        // connect to the ring for incoming packets
        std::string upstream_ring_name = ring_name_str(config_.upstream_core, socket_id_, proc_idx_);
        struct rte_ring* upstream_ring = rte_ring_lookup(upstream_ring_name.c_str());
        if (upstream_ring == NULL)
        {
            // this needs to error out as there should always be upstream resources at this point
            LOG4CXX_INFO(logger_, config_.core_name << " : " << proc_idx_ << " Failed to Connect to upstream resources!");
            return false;
        }
        else
        {
            upstream_ring_ = upstream_ring;
            LOG4CXX_DEBUG_LEVEL(2, logger_, "Frame ready ring with name "
                << upstream_ring_name << " has already been created"
            );  
        }

        // connect to the ring for new memory locations packets
        std::string clear_frames_ring_name = ring_name_clear_frames(socket_id_);
        clear_frames_ring_ = rte_ring_lookup(clear_frames_ring_name.c_str());
        if (clear_frames_ring_ == NULL)
        {
            // this needs to error out as there should always be upstream resources at this point
            LOG4CXX_INFO(logger_, config_.core_name << " : " << proc_idx_ << " Failed to Connect to upstream resources!");
            return false;
        }
        else
        {
            LOG4CXX_DEBUG_LEVEL(2, logger_, "Frame ready ring with name "
                << upstream_ring_name << " has already been created"
            );  
        }

        LOG4CXX_INFO(logger_, config_.core_name << " : " << proc_idx_ << " Connected to upstream resources successfully!");

        return true;
    }

    void FrameWrapperCore::configure(OdinData::IpcMessage& config)
    {
        // Update the config based from the passed IPCmessage

        LOG4CXX_INFO(logger_, config_.core_name << " : " << proc_idx_ << " Got update config.");

    }

    DPDKREGISTER(DpdkWorkerCore, FrameWrapperCore, "FrameWrapperCore");
}
