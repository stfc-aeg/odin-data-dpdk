#include "FrameBuilderCore.h"
#include "DpdkSharedBufferFrame.h"
#include "DpdkUtils.h"
#include <stdexcept>

namespace FrameProcessor
{
    FrameBuilderCore::FrameBuilderCore(
        int fb_idx, int socket_id, DpdkWorkCoreReferences &dpdkWorkCoreReferences
    ) :
        DpdkWorkerCore(socket_id),
        logger_(Logger::getLogger("FP.FrameBuilderCore")),
        proc_idx_(fb_idx),
        decoder_(dynamic_cast<PacketProtocolDecoder *>(dpdkWorkCoreReferences.decoder)),
        shared_buf_(dpdkWorkCoreReferences.shared_buf),
        built_frames_(0),
        built_frames_hz_(0),
        idle_loops_(0),
        mean_us_on_frame_(0),
        maximum_us_on_frame_(0),
        core_usage_(0)
    {
        config_.resolve(dpdkWorkCoreReferences.core_config);

        LOG4CXX_INFO(logger_, "FP.FrameBuilderCore " << proc_idx_ << " Created with config:"
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

    FrameBuilderCore::~FrameBuilderCore(void)
    {
        LOG4CXX_DEBUG_LEVEL(2, logger_, "FrameBuilderCore destructor");
        stop();
        for (auto& ring : downstream_rings_)
        {
            if (ring) rte_ring_free(ring);
        }
        downstream_rings_.clear();
    }

    bool FrameBuilderCore::run(unsigned int lcore_id)
    {

        lcore_id_ = lcore_id;
        run_lcore_ = true;

        LOG4CXX_INFO(logger_, "Core " << lcore_id_ << " starting up");

        struct SuperFrameHeader *current_frame_buffer_;
        struct SuperFrameHeader *reordered_frame_location_;
        struct SuperFrameHeader *returned_frame_location_;

        dimensions_t dims(2);
        dims[0] = decoder_->get_frame_x_resolution();
        dims[1] = decoder_->get_frame_y_resolution();
        std::size_t frame_size = dims[0] * dims[1] * get_size_from_enum(decoder_->get_frame_bit_depth());
        std::size_t payload_size = decoder_->get_payload_size();

        uint64_t frames_per_second = 1;
        uint64_t last = rte_get_tsc_cycles();
        uint64_t cycles_per_sec = rte_get_tsc_hz();
        uint64_t cycles_working = 1;
        uint64_t start_frame_cycles = 1;
        uint64_t total_frame_cycles = 1;
        uint64_t maximum_frame_cycles = 1;

        // Reserve a hugepages buffer for the reordered frame output
        rte_ring_dequeue(clear_frames_ring_, (void **)&reordered_frame_location_);

        // While loop to continuously dequeue frame objects
        while (likely(run_lcore_))
        {
            uint64_t now = rte_get_tsc_cycles();
            if (unlikely((now - last) >= (cycles_per_sec)))
            {
                // Update any monitoring variables every second
                built_frames_hz_ = frames_per_second - 1;
                mean_us_on_frame_ = (total_frame_cycles * 1000000) / (frames_per_second * cycles_per_sec);
                core_usage_ = (cycles_working * 255) / cycles_per_sec;

                maximum_us_on_frame_ = (maximum_frame_cycles * 1000000) / (cycles_per_sec);

                // Reset any counters
                frames_per_second = 1;
                idle_loops_ = 0;
                total_frame_cycles = 1;
                cycles_working = 1;
                last = now;
            }
            // Attempt to dequeue a new frame object
            if (rte_ring_dequeue(upstream_ring_, (void **)&current_frame_buffer_) < 0)
            {
                // No frame was dequeued, try again
                idle_loops_++;
                continue;
            }
            else
            {
                start_frame_cycles = rte_get_tsc_cycles();

                uint64_t frame_number = decoder_->get_super_frame_number(current_frame_buffer_);

                LOG4CXX_DEBUG(logger_, config_.core_name << " : " << proc_idx_ << " Got frame: " << frame_number);

                // Zero out payload slots for any dropped packets to prevent stale data from
                // a previous acquisition from leaking into this frame (memory is reused)
                uint32_t incomplete_frames = decoder_->get_frame_outer_chunk_size() -
                    decoder_->get_super_frame_frames_received(current_frame_buffer_);

                if (incomplete_frames)
                {
                    uint32_t frame_idx = 0;
                    uint32_t frames_cleared = 0;

                    while (frames_cleared < incomplete_frames)
                    {
                        uint32_t packet_idx = 0;
                        uint32_t packets_cleared = 0;

                        uint32_t packets_dropped = decoder_->get_packets_dropped(
                            decoder_->get_frame_header(current_frame_buffer_, frame_idx)
                        );

                        while (packets_cleared < packets_dropped)
                        {
                            if (decoder_->get_packet_state(decoder_->get_frame_header(current_frame_buffer_, frame_idx), packet_idx) == 0)
                            {
                                memset(
                                    decoder_->get_image_data_start(current_frame_buffer_) +
                                        (frame_idx * payload_size * decoder_->get_packets_per_frame()) +
                                        (packet_idx * payload_size),
                                    0, payload_size
                                );
                                packets_cleared++;
                            }
                            packet_idx++;
                        }
                        frames_cleared++;
                    }
                }

                returned_frame_location_ =
                    decoder_->reorder_frame(current_frame_buffer_, reordered_frame_location_);

                decoder_->set_super_frame_image_size(
                    returned_frame_location_, frame_size * decoder_->get_frame_outer_chunk_size()
                );

                rte_ring_enqueue(
                    downstream_rings_[frame_number % config_.num_downstream_cores], returned_frame_location_
                );

                // If reorder_frame() wrote into reordered_frame_location_, recycle the source buffer
                if (returned_frame_location_ == reordered_frame_location_)
                {
                    reordered_frame_location_ = current_frame_buffer_;
                }

                uint64_t cycles_spent = rte_get_tsc_cycles() - start_frame_cycles;
                total_frame_cycles += cycles_spent;
                cycles_working += cycles_spent;
                
                if (maximum_frame_cycles < cycles_spent)
                {
                    maximum_frame_cycles = cycles_spent;
                }
                

                frames_per_second++;
                built_frames_++;

                LOG4CXX_DEBUG(logger_, config_.core_name << " : " << proc_idx_ << " Built frame: " << frame_number);
            }
        }

        LOG4CXX_INFO(logger_, "Core " << lcore_id_ << " completed");

        return true;
    }

    void FrameBuilderCore::stop(void)
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

    void FrameBuilderCore::status(OdinData::IpcMessage &status, const std::string &path)
    {
        std::string status_path = path + "/framebuildercore_" + std::to_string(proc_idx_) + "/";
        std::string ring_status = status_path + "upstream_rings/";
        std::string timing_status = status_path + "timing/";

        status.set_param(status_path + "frames_processed", built_frames_);
        status.set_param(status_path + "frames_processed_per_second", built_frames_hz_);
        status.set_param(status_path + "idle_loops", idle_loops_);
        status.set_param(status_path + "core_usage", (int)core_usage_);

        status.set_param(timing_status + "mean_frame_us", mean_us_on_frame_);
        status.set_param(timing_status + "max_frame_us", maximum_us_on_frame_);

        status.set_param(ring_status + ring_name_str(config_.upstream_core, socket_id_, proc_idx_) + "_count", rte_ring_count(upstream_ring_));
        status.set_param(ring_status + ring_name_str(config_.upstream_core, socket_id_, proc_idx_) + "_size", rte_ring_get_size(upstream_ring_));
    }

    bool FrameBuilderCore::connect(void)
    {
        std::string upstream_ring_name = ring_name_str(config_.upstream_core, socket_id_, proc_idx_);
        upstream_ring_ = rte_ring_lookup(upstream_ring_name.c_str());
        if (upstream_ring_ == NULL)
        {
            LOG4CXX_ERROR(logger_, config_.core_name << " : " << proc_idx_ << " Failed to connect to upstream ring: " << upstream_ring_name);
            return false;
        }

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

    void FrameBuilderCore::configure(OdinData::IpcMessage& config)
    {
        LOG4CXX_INFO(logger_, config_.core_name << " : " << proc_idx_ << " Got update config.");
    }

    DPDKREGISTER(DpdkWorkerCore, FrameBuilderCore, "FrameBuilderCore");
}
