#include "RegionOfInterestCore.h"
#include "DpdkUtils.h"
#include <stdexcept>

namespace FrameProcessor
{
    RegionOfInterestCore::RegionOfInterestCore(
        int fb_idx, int socket_id, DpdkWorkCoreReferences &dpdkWorkCoreReferences
    ) :
        DpdkWorkerCore(socket_id),
        logger_(Logger::getLogger("FP.RegionOfInterestCore")),
        proc_idx_(fb_idx),
        decoder_(dpdkWorkCoreReferences.decoder),
        mode_(dpdkWorkCoreReferences.decoder_mode),
        shared_buf_(dpdkWorkCoreReferences.shared_buf),
        last_frame_(-1),
        processed_frames_(0),
        processed_frames_hz_(0),
        idle_loops_(0),
        mean_us_on_frame_(0),
        maximum_us_on_frame_(0),
        core_usage_(0)
    {
        config_.resolve(dpdkWorkCoreReferences.core_config, dpdkWorkCoreReferences.config_key);

        LOG4CXX_INFO(logger_, "FP.RegionOfInterestCore " << proc_idx_ << " Created with config:"
            << " | core_name: " << config_.core_name
            << " | num_cores: " << config_.num_cores
            << " | connect: " << config_.connect
            << " | upstream_core: " << config_.upstream_core
            << " | num_downstream_cores: " << config_.num_downstream_cores
            << " | roi: (" << config_.roi_x0_ << ", " << config_.roi_y0_ << ") -> ("
                << config_.roi_x1_ << ", " << config_.roi_y1_ << ")"
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
    }

    RegionOfInterestCore::~RegionOfInterestCore(void)
    {
        LOG4CXX_DEBUG_LEVEL(2, logger_, "RegionOfInterestCore destructor");
        stop();
    }

    bool RegionOfInterestCore::run(unsigned int lcore_id)
    {

        lcore_id_ = lcore_id;
        run_lcore_ = true;

        LOG4CXX_INFO(logger_, "Core " << lcore_id_ << " starting up");

        struct SuperFrameHeader *current_frame_buffer_, *roi_frame_ = nullptr;

        std::size_t full_frame_x_resolution = decoder_->get_frame_x_resolution(mode_);
        std::size_t bytes_per_pixel = get_size_from_enum(decoder_->get_frame_bit_depth(mode_));

        // Crop box is specified as top-left (x0, y0) and bottom-right (x1, y1) coordinates
        std::size_t roi_x0 = config_.roi_x0_;
        std::size_t roi_y0 = config_.roi_y0_;
        std::size_t roi_width = config_.roi_x1_ - config_.roi_x0_;
        std::size_t roi_height = config_.roi_y1_ - config_.roi_y0_;

        std::size_t frames_per_super_frame = decoder_->get_frame_outer_chunk_size(mode_);

        std::size_t row_bytes = roi_width * bytes_per_pixel;
        std::size_t roi_frame_size = roi_width * roi_height * bytes_per_pixel * frames_per_super_frame;
        std::size_t full_row_bytes = full_frame_x_resolution * bytes_per_pixel;

        // Distance between the start of one sub-frame's image data and the next, in the
        // uncropped source buffer, and in the cropped destination buffer respectively
        std::size_t src_sub_frame_stride = full_frame_x_resolution * decoder_->get_frame_y_resolution(mode_)
            * bytes_per_pixel;
        std::size_t dst_sub_frame_stride = roi_width * roi_height * bytes_per_pixel;

        // All per-sub-frame headers are packed contiguously after the super-frame header
        std::size_t header_bytes = decoder_->get_super_frame_header_size()
            + (decoder_->get_frame_header_size(mode_) * frames_per_super_frame);

        uint64_t frames_per_second = 1;
        uint64_t last = rte_get_tsc_cycles();
        uint64_t cycles_per_sec = rte_get_tsc_hz();
        uint64_t cycles_working = 1;
        uint64_t start_frame_cycles = 1;
        uint64_t total_frame_cycles = 1;
        uint64_t maximum_frame_cycles = 1;
        uint64_t idle_loops = 0;

        // Reserve a hugepages buffer to hold the cropped output
        while (roi_frame_ == NULL)
        {
            rte_ring_dequeue(clear_frames_ring_, (void**) &roi_frame_);
        }

        LOG4CXX_INFO(logger_, "roi: (" << roi_x0 << ", " << roi_y0 << ") "
            << roi_width << "x" << roi_height
            << " bytes_per_pixel: " << bytes_per_pixel
            << " roi_frame_size: " << roi_frame_size);

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
                last_frame_ = frame_number;

                // Copy the super-frame header and frame header verbatim into the output buffer
                rte_memcpy(roi_frame_, current_frame_buffer_, header_bytes);

                // Copy the selected rows within the ROI, row by row, for every sub-frame packed
                // into this super frame
                char* src_image = decoder_->get_image_data_start(current_frame_buffer_, mode_);
                char* dst_image = decoder_->get_image_data_start(roi_frame_, mode_);

                for (std::size_t sub_frame = 0; sub_frame < frames_per_super_frame; sub_frame++)
                {
                    char* src_sub_frame = src_image + (sub_frame * src_sub_frame_stride);
                    char* dst_sub_frame = dst_image + (sub_frame * dst_sub_frame_stride);

                    for (std::size_t row = 0; row < roi_height; row++)
                    {
                        char* src_row = src_sub_frame + ((roi_y0 + row) * full_row_bytes) + (roi_x0 * bytes_per_pixel);
                        char* dst_row = dst_sub_frame + (row * row_bytes);
                        rte_memcpy(dst_row, src_row, row_bytes);
                    }
                }

                decoder_->set_super_frame_image_size(roi_frame_, roi_frame_size);
                decoder_->set_super_frame_x_resolution(roi_frame_, roi_width);
                decoder_->set_super_frame_y_resolution(roi_frame_, roi_height);

                rte_ring_enqueue(downstream_rings_[frame_number % config_.num_downstream_cores], roi_frame_);

                // Reuse the now-consumed source buffer as the destination for the next frame
                roi_frame_ = current_frame_buffer_;

                uint64_t cycles_spent = rte_get_tsc_cycles() - start_frame_cycles;
                total_frame_cycles += cycles_spent;
                cycles_working += cycles_spent;

                if (maximum_frame_cycles < cycles_spent)
                {
                    maximum_frame_cycles = cycles_spent;
                }

                frames_per_second++;
                processed_frames_++;

                LOG4CXX_DEBUG(logger_, config_.core_name << " : " << proc_idx_ << " Cropped frame: " << frame_number);
            }
        }

        LOG4CXX_INFO(logger_, "Core " << lcore_id_ << " completed");

        return true;
    }

    void RegionOfInterestCore::stop(void)
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

    void RegionOfInterestCore::status(OdinData::IpcMessage& status, const std::string& path)
    {
        std::string status_path = path + "/RegionOfInterestCore_" + std::to_string(proc_idx_) + "/";
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

    bool RegionOfInterestCore::connect(void)
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

    void RegionOfInterestCore::configure(OdinData::IpcMessage& config)
    {
        LOG4CXX_INFO(logger_, config_.core_name << " : " << proc_idx_ << " Got update config.");
    }

    void RegionOfInterestCore::requestConfiguration(OdinData::IpcMessage& reply)
    {
        LOG4CXX_DEBUG(logger_, "Configuration requested for worker core");
    }

    DPDKREGISTER(DpdkWorkerCore, RegionOfInterestCore, "RegionOfInterestCore");
}
