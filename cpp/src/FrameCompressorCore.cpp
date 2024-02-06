#include "FrameCompressorCore.h"
#include "DpdkUtils.h"
#include <blosc.h>
#include "DpdkSharedBufferFrame.h"
#include <iostream>
#include <string>

namespace FrameProcessor
{
    FrameCompressorCore::FrameCompressorCore(
        int fb_idx, int socket_id, DpdkWorkCoreReferences &dpdkWorkCoreReferences
    ) :
        DpdkWorkerCore(socket_id),
        logger_(Logger::getLogger("FP.FrameCompressorCore")),
        proc_idx_(fb_idx),
        decoder_(dpdkWorkCoreReferences.decoder),
        shared_buf_(dpdkWorkCoreReferences.shared_buf),
        built_frames_(0),
        built_frames_hz_(0),
        idle_loops_(0),
        avg_us_spent_compressing_(0)
    {

        // Get the configuration container for this worker
        config_.resolve(dpdkWorkCoreReferences.core_config);

        LOG4CXX_INFO(logger_, "FP.FrameCompressorCore " << proc_idx_ << " Created with config:"
            << " | core_name" << config_.core_name
            << " | num_cores: " << config_.num_cores
            << " | connect: " << config_.connect
            << " | upstream_core: " << config_.upstream_core
            << " | num_downsteam_cores: " << config_.num_downstream_cores
        );

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

    }

    FrameCompressorCore::~FrameCompressorCore(void)
    {
        LOG4CXX_DEBUG_LEVEL(2, logger_, "FrameCompressorCore destructor");
        stop();
    }

    bool FrameCompressorCore::run(unsigned int lcore_id)
    {

        lcore_id_ = lcore_id;
        run_lcore_ = true;

        LOG4CXX_INFO(logger_, "Core " << lcore_id_ << " starting up");

        // Blosc compression settings
        const char * p_compressor_name;
        blosc_compcode_to_compname(1, &p_compressor_name);

        // Generic frame variables
        struct RawFrameHeader *current_frame_buffer_, *compressed_frame_;
        dimensions_t dims(2);
        int compressed_size = 0;

        // Specific frame variables from decoder
        dims[0] = decoder_->get_frame_x_resolution();
        dims[1] = decoder_->get_frame_y_resolution();
        std::size_t frame_size = 
            dims[0] * dims[1] * get_size_from_enum(decoder_->get_frame_bit_depth());
        size_t dest_data_size = frame_size + BLOSC_MAX_OVERHEAD;
        std::size_t frame_header_size = decoder_->get_frame_header_size();

        // Status reporting variables
        uint64_t frames_per_second = 0;
        uint64_t last = rte_get_tsc_cycles();
        uint64_t cycles_per_sec = rte_get_tsc_hz();
        uint64_t start_compressing = 1;
        uint64_t average_compression_cycles = 1;

        while (compressed_frame_ == NULL)
        {
            rte_ring_dequeue(clear_frames_ring_, (void**) &compressed_frame_);
        }

        //While loop to continuously dequeue frame objects
        while (likely(run_lcore_))
        {
            uint64_t now = rte_get_tsc_cycles();
            if (unlikely((now - last) >= (cycles_per_sec)))
            {
                // Update any monitoring variables every second
                built_frames_hz_ = frames_per_second;
                avg_us_spent_compressing_ = (average_compression_cycles * 1000000 )/ cycles_per_sec;

                // Reset any counters
                frames_per_second = 0;
                idle_loops_ = 0;
                average_compression_cycles = 0;

                last = now;
            }
            // Attempt to dequeue a new frame object
            if (rte_ring_dequeue(upstream_ring_, (void**) &current_frame_buffer_) < 0)
            {

                // No frame was dequeued, try again
                idle_loops_++;
                continue;
            }
            else
            {
                built_frames_++;
                uint64_t frame_number = decoder_->get_frame_number(current_frame_buffer_);

                start_compressing = rte_get_tsc_cycles();

                // Compress the cores using the provided config
                compressed_size = blosc_compress_ctx(
                    1, 1,
                    get_size_from_enum(decoder_->get_frame_bit_depth()), frame_size,
                    reinterpret_cast<char *>(current_frame_buffer_) + frame_header_size,
                    reinterpret_cast<char *>(compressed_frame_) + frame_header_size, dest_data_size, p_compressor_name,
                    0, 1
                );

                // Copy the Frame_Header to the new memory location
                rte_memcpy(compressed_frame_, current_frame_buffer_, frame_header_size);

                // Set the correct image size to ensure that correct data is saved out
                decoder_->set_image_size(compressed_frame_, compressed_size);

                // Enqueue the frame to be wrapped into a shared pointer
                rte_ring_enqueue(downstream_rings_[frame_number % (config_.num_downstream_cores)], compressed_frame_);

                average_compression_cycles = 
                    (average_compression_cycles + (rte_get_tsc_cycles() - start_compressing)) / 2;

                // Resuse the old frame location for the next frame to be compressed
                compressed_frame_ = current_frame_buffer_;
                frames_per_second++;
                
            }
        }

        LOG4CXX_INFO(logger_, "Core " << lcore_id_ << " completed");

        return true;
    }

    void FrameCompressorCore::stop(void)
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

    void FrameCompressorCore::status(OdinData::IpcMessage& status, const std::string& path)
    {
        LOG4CXX_DEBUG(logger_, "Status requested for FrameCompressorCore_" << proc_idx_
            << " from the DPDK plugin");

        std::string status_path = path + "/FrameCompressorCore_" + std::to_string(proc_idx_) + "/";

        status.set_param(status_path + "frames_built", built_frames_);
        status.set_param(status_path + "frames_built_hz", built_frames_hz_);
        status.set_param(status_path + "idle_loops", idle_loops_);
        status.set_param(status_path + "average_us_compressing", avg_us_spent_compressing_);
    }

    bool FrameCompressorCore::connect(void)
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

    DPDKREGISTER(DpdkWorkerCore, FrameCompressorCore, "FrameCompressorCore");
}