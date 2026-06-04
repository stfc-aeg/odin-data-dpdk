#include "FrameCompressorCore.h"
#include "DpdkUtils.h"
#include <blosc.h>
#include <stdexcept>

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
        processed_frames_(0),
        processed_frames_hz_(0),
        idle_loops_(0),
        mean_us_on_frame_(0),
        maximum_us_on_frame_(0),
        core_usage_(0),
        last_frame_(-1)
    {
        config_.resolve(dpdkWorkCoreReferences.core_config);

        LOG4CXX_INFO(logger_, "FP.FrameCompressorCore " << proc_idx_ << " Created with config:"
            << " | core_name: " << config_.core_name
            << " | num_cores: " << config_.num_cores
            << " | connect: " << config_.connect
            << " | upstream_core: " << config_.upstream_core
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

        const char *p_compressor_name;
        blosc_compcode_to_compname(1, &p_compressor_name);

        struct SuperFrameHeader *current_frame_buffer_, *compressed_frame_ = nullptr;
        dimensions_t dims(2);
        int compressed_size = 0;

        dims[0] = decoder_->get_frame_x_resolution();
        dims[1] = decoder_->get_frame_y_resolution();
        std::size_t frame_size = dims[0] * dims[1] * get_size_from_enum(decoder_->get_frame_bit_depth());
        size_t dest_data_size = frame_size + BLOSC_MAX_OVERHEAD;

        uint64_t frames_per_second = 1;
        uint64_t last = rte_get_tsc_cycles();
        uint64_t cycles_per_sec = rte_get_tsc_hz();
        uint64_t cycles_working = 1;
        uint64_t start_frame_cycles = 1;
        uint64_t total_frame_cycles = 1;
        uint64_t maximum_frame_cycles = 1;
        uint64_t idle_loops = 0;

        // Reserve a hugepages buffer to hold the compressed output
        while (compressed_frame_ == NULL)
        {
            rte_ring_dequeue(clear_frames_ring_, (void**) &compressed_frame_);
        }

        LOG4CXX_INFO(logger_, "frame dims: " << dims[0] << "x" << dims[1]
            << " bit_depth: " << (int)decoder_->get_frame_bit_depth()
            << " frame_size: " << frame_size
            << " dest_data_size: " << dest_data_size
            << " buffer_size: " << shared_buf_->get_buffer_size());


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
            if (rte_ring_dequeue(upstream_ring_, (void**) &current_frame_buffer_) < 0)
            {
                idle_loops_++;
                continue;
            }
            else
            {
                start_frame_cycles = rte_get_tsc_cycles();

                uint64_t frame_number = decoder_->get_super_frame_number(current_frame_buffer_);

                compressed_size = blosc_compress_ctx(
                    1, 1,
                    get_size_from_enum(decoder_->get_frame_bit_depth()), frame_size,
                    decoder_->get_image_data_start(current_frame_buffer_),
                    decoder_->get_image_data_start(compressed_frame_), dest_data_size, p_compressor_name,
                    0, 1
                );

                // Copy the super-frame header into the compressed output buffer before forwarding
                rte_memcpy(compressed_frame_, current_frame_buffer_,
                    decoder_->get_super_frame_header_size() + decoder_->get_frame_header_size()
                );

                decoder_->set_super_frame_image_size(compressed_frame_, compressed_size);

                rte_ring_enqueue(downstream_rings_[frame_number % config_.num_downstream_cores], compressed_frame_);

                // Reuse the now-consumed source buffer as the destination for the next frame
                compressed_frame_ = current_frame_buffer_;

                uint64_t cycles_spent = rte_get_tsc_cycles() - start_frame_cycles;
                total_frame_cycles += cycles_spent;
                cycles_working += cycles_spent;

                if (maximum_frame_cycles < cycles_spent)
                {
                    maximum_frame_cycles = cycles_spent;
                }

                frames_per_second++;
                processed_frames_++;

                LOG4CXX_DEBUG(logger_, config_.core_name << " : " << proc_idx_ << " Compressed frame: " << frame_number);
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
        std::string status_path = path + "/FrameCompressorCore_" + std::to_string(proc_idx_) + "/";
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

        status.set_param(ring_status + ring_name_clear_frames(socket_id_) + "_count", rte_ring_count(clear_frames_ring_));
        status.set_param(ring_status + ring_name_clear_frames(socket_id_) + "_size", rte_ring_get_size(clear_frames_ring_));
    }

    bool FrameCompressorCore::connect(void)
    {
        std::string upstream_ring_name = ring_name_str(config_.upstream_core, socket_id_, proc_idx_);
        struct rte_ring* upstream_ring = rte_ring_lookup(upstream_ring_name.c_str());
        if (upstream_ring == NULL)
        {
            LOG4CXX_ERROR(logger_, config_.core_name << " : " << proc_idx_ << " Failed to connect to upstream ring: " << upstream_ring_name);
            return false;
        }
        upstream_ring_ = upstream_ring;

        std::string clear_frames_ring_name = ring_name_clear_frames(socket_id_);
        clear_frames_ring_ = rte_ring_lookup(clear_frames_ring_name.c_str());
        if (clear_frames_ring_ == NULL)
        {
            LOG4CXX_ERROR(logger_, config_.core_name << " : " << proc_idx_ << " Failed to connect to clear_frames ring: " << clear_frames_ring_name);
            return false;
        }

        LOG4CXX_INFO(logger_, config_.core_name << " : " << proc_idx_ << " Connected to upstream resources successfully!");

        return true;
    }

    void FrameCompressorCore::configure(OdinData::IpcMessage& config)
    {
        LOG4CXX_INFO(logger_, config_.core_name << " : " << proc_idx_ << " Got update config.");
    }

    DPDKREGISTER(DpdkWorkerCore, FrameCompressorCore, "FrameCompressorCore");
}