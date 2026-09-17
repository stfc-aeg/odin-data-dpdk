#include "FrameWrapperCore.h"
#include "DpdkUtils.h"
#include <blosc.h>
#include "DpdkSharedBufferFrame.h"

namespace FrameProcessor
{
    FrameWrapperCore::FrameWrapperCore(
        int fb_idx, int socket_id, DpdkWorkCoreReferences &dpdkWorkCoreReferences
    ) :
        DpdkWorkerCore(socket_id),
        logger_(Logger::getLogger("FP.FrameWrapperCore")),
        proc_idx_(fb_idx),
        decoder_(dpdkWorkCoreReferences.decoder),
        mode_(dpdkWorkCoreReferences.decoder_mode),
        frame_callback_(dpdkWorkCoreReferences.frame_callback),
        processed_frames_(0),
        processed_frames_hz_(0),
        idle_loops_(0),
        mean_us_on_frame_(0),
        maximum_us_on_frame_(0),
        core_usage_(0),
        last_frame_(-1)
    {
        config_.resolve(dpdkWorkCoreReferences.core_config, dpdkWorkCoreReferences.config_key);

        LOG4CXX_INFO(logger_, "FP.FrameWrapperCore " << proc_idx_ << " Created with config:"
            << " | core_name: " << config_.core_name
            << " | num_cores: " << config_.num_cores
            << " | connect: " << config_.connect
            << " | upstream_core: " << config_.upstream_core
            << " | num_downstream_cores: " << config_.num_downstream_cores
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

        const char *p_compressor_name;
        blosc_compcode_to_compname(1, &p_compressor_name);

        struct SuperFrameHeader *current_super_frame_buffer_;

        std::vector<std::size_t> decoder_dims = decoder_->get_frame_dimensions(mode_);
        dimensions_t dims(decoder_dims.size());
        for (size_t i = 0; i < decoder_dims.size(); i++)
        {
            dims[i] = decoder_dims[i];
        }

        uint64_t data_pointer_offset = decoder_->get_image_data_offset(mode_);

        uint64_t frames_per_second = 1;
        uint64_t last = rte_get_tsc_cycles();
        uint64_t cycles_per_sec = rte_get_tsc_hz();
        uint64_t cycles_working = 1;
        uint64_t start_frame_cycles = 1;
        uint64_t total_frame_cycles = 1;
        uint64_t maximum_frame_cycles = 1;
        uint64_t idle_loops = 0;

        while (likely(run_lcore_))
        {
            uint64_t now = rte_get_tsc_cycles();
            if (unlikely((now - last) >= (cycles_per_sec)))
            {
                // Update any monitoring variables every second
                processed_frames_hz_ = frames_per_second - 1;
                mean_us_on_frame_ = (total_frame_cycles * 1000000) / (frames_per_second * cycles_per_sec);
                core_usage_ = (cycles_working * 255) / cycles_per_sec;

                maximum_us_on_frame_ = (maximum_frame_cycles * 1000000) / (cycles_per_sec);

                idle_loops_ = idle_loops;

                // Reset any counters
                frames_per_second = 1;
                idle_loops = 0;
                total_frame_cycles = 1;
                cycles_working = 1;
                last = now;
            }
            if (rte_ring_dequeue(upstream_ring_, (void**) &current_super_frame_buffer_) < 0)
            {
                idle_loops++;
                continue;
            }
            else
            {
                start_frame_cycles = rte_get_tsc_cycles();

                uint64_t frame_number = decoder_->get_super_frame_number(current_super_frame_buffer_);
                last_frame_ = frame_number;

                // Actual per-frame dimensions, which may differ from the decoder mode's native
                // resolution if an upstream core (e.g. RegionOfInterestCore) has resized the frame
                dimensions_t actual_dims(dims.size());
                actual_dims[0] = decoder_->get_super_frame_y_resolution(current_super_frame_buffer_);
                actual_dims[1] = decoder_->get_super_frame_x_resolution(current_super_frame_buffer_);
                for (size_t i = 2; i < dims.size(); i++)
                {
                    actual_dims[i] = dims[i];
                }

                FrameMetaData frame_meta;
                frame_meta.set_dataset_name(config_.dataset_name_);
                frame_meta.set_frame_number(frame_number);
                frame_meta.set_dimensions(actual_dims);
                frame_meta.set_data_type(decoder_->get_frame_bit_depth(mode_));

                // Expected uncompressed size for this specific frame's actual dimensions, which may
                // differ from the decoder mode's native resolution if the frame was resized upstream
                std::size_t actual_frame_size = 1;
                for (const auto& dim : actual_dims)
                {
                    actual_frame_size *= dim;
                }
                actual_frame_size *= decoder_->get_frame_outer_chunk_size(mode_) *
                    (decoder_->get_frame_bit_depth(mode_) == FrameProcessor::DataType::raw_64bit ? 8 :
                    decoder_->get_frame_bit_depth(mode_) == FrameProcessor::DataType::raw_32bit ? 4 :
                    decoder_->get_frame_bit_depth(mode_) == FrameProcessor::DataType::raw_16bit ? 2 : 1);

                // image_size differs from actual_frame_size when FrameCompressorCore has compressed the frame
                uint64_t image_size = decoder_->get_super_frame_image_size(current_super_frame_buffer_);
                frame_meta.set_compression_type(actual_frame_size != image_size ? blosc : no_compression);

                // Wrap the hugepages buffer in a shared Frame; the destructor returns it to clear_frames_ring_
                boost::shared_ptr<Frame> complete_frame =
                    boost::shared_ptr<Frame>(new DpdkSharedBufferFrame(
                        frame_meta, current_super_frame_buffer_,
                        decoder_->get_frame_buffer_size(mode_),
                        clear_frames_ring_, data_pointer_offset
                    ));


                // Check if the frame number if between 0 and 99, if not print the frame number as this and error
                if (frame_number < 0 || frame_number > 99999999999)
                {
                    LOG4CXX_ERROR(logger_, "Wrapped frame: " << frame_number << " mode: " << mode_);
                }

                complete_frame->set_image_size(image_size);
                complete_frame->set_outer_chunk_size(decoder_->get_frame_outer_chunk_size(mode_));
                frame_callback_(complete_frame);

                uint64_t cycles_spent = rte_get_tsc_cycles() - start_frame_cycles;
                total_frame_cycles += cycles_spent;
                cycles_working += cycles_spent;

                if (maximum_frame_cycles < cycles_spent)
                {
                    maximum_frame_cycles = cycles_spent;
                }

                frames_per_second++;
                processed_frames_++;

                LOG4CXX_DEBUG(logger_, config_.core_name << " : " << proc_idx_ << " Wrapped frame: " << frame_number);
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
        std::string status_path = path + "/FrameWrapperCore_" + std::to_string(proc_idx_) + "/";
        std::string ring_status = status_path + "upstream_rings/";
        std::string timing_status = status_path + "timing/";

        status.set_param(status_path + "frames_processed", processed_frames_);
        status.set_param(status_path + "frames_processed_per_second", processed_frames_hz_);
        status.set_param(status_path + "idle_loops", idle_loops_);
        status.set_param(status_path + "core_usage", (int)core_usage_);
        status.set_param(status_path + "last_frame_number", last_frame_);

        status.set_param(timing_status + "mean_frame_us", mean_us_on_frame_);
        status.set_param(timing_status + "max_frame_us", maximum_us_on_frame_);

        status.set_param(ring_status + ring_name_str(config_.upstream_core, socket_id_, proc_idx_) + "_count", rte_ring_count(upstream_ring_));
        status.set_param(ring_status + ring_name_str(config_.upstream_core, socket_id_, proc_idx_) + "_size", rte_ring_get_size(upstream_ring_));

        status.set_param(ring_status + ring_name_clear_frames(socket_id_, config_.stream_id) + "_count", rte_ring_count(clear_frames_ring_));
        status.set_param(ring_status + ring_name_clear_frames(socket_id_, config_.stream_id) + "_size", rte_ring_get_size(clear_frames_ring_));
    }

    bool FrameWrapperCore::connect(void)
    {
        std::string upstream_ring_name = ring_name_str(config_.upstream_core, socket_id_, proc_idx_);
        struct rte_ring* upstream_ring = rte_ring_lookup(upstream_ring_name.c_str());
        if (upstream_ring == NULL)
        {
            LOG4CXX_ERROR(logger_, config_.core_name << " : " << proc_idx_ << " Failed to connect to upstream ring: " << upstream_ring_name);
            return false;
        }
        upstream_ring_ = upstream_ring;

        std::string clear_frames_ring_name = ring_name_clear_frames(socket_id_, config_.stream_id);
        clear_frames_ring_ = rte_ring_lookup(clear_frames_ring_name.c_str());
        if (clear_frames_ring_ == NULL)
        {
            LOG4CXX_ERROR(logger_, config_.core_name << " : " << proc_idx_ << " Failed to connect to clear_frames ring: " << clear_frames_ring_name);
            return false;
        }

        LOG4CXX_INFO(logger_, config_.core_name << " : " << proc_idx_ << " Connected to upstream resources successfully!");

        return true;
    }

    void FrameWrapperCore::configure(OdinData::IpcMessage& config)
    {
        LOG4CXX_INFO(logger_, config_.core_name << " : " << proc_idx_ << " Got update config.");
    }

    void FrameWrapperCore::requestConfiguration(OdinData::IpcMessage& reply)
    {
        LOG4CXX_DEBUG(logger_, "Configuration requested for worker core");
    }

    DPDKREGISTER(DpdkWorkerCore, FrameWrapperCore, "FrameWrapperCore");
}
