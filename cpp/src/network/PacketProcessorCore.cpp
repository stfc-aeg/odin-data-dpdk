#include "network/PacketProcessorCore.h"

#include <iostream>
#include <stdexcept>
#include <unordered_map>

#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_udp.h>
#include <rte_malloc.h>
#include <rte_prefetch.h>
#include "DpdkUtils.h"

namespace FrameProcessor
{
    PacketProcessorCore::PacketProcessorCore(
        int proc_idx, int socket_id, DpdkWorkCoreReferences dpdkWorkCoreReferences
    ) :
        DpdkWorkerCore(socket_id),
        proc_idx_(proc_idx),
        decoder_(dynamic_cast<PacketProtocolDecoder *>(dpdkWorkCoreReferences.decoder)),
        shared_buf_(dpdkWorkCoreReferences.shared_buf),
        dropped_frames_(0),
        dropped_packets_(0),
        current_frame_(-1),
        incomplete_frames_(0),
        last_frame_(0),
        processed_frames_(0),
        processed_frames_hz_(0),
        idle_loops_(0),
        mean_us_on_frame_(0),
        maximum_us_on_frame_(0),
        core_usage_(0),
        first_frame_number_(-1),
        total_packets_(0),
        logger_(Logger::getLogger("FP.PacketProcCore"))
    {

        // Resolve configuration parameters for this core from the config object passed as an
        // argument, and the current port ID
        config_.resolve(dpdkWorkCoreReferences.core_config);

        // Determine debug level for performance-critical logging
        debug_enabled_ = false;
        trace_enabled_ = false;

        LOG4CXX_INFO(logger_, "FP.PacketProcCore " << proc_idx_ << " Created with config:"
            << " | core_name: " << config_.core_name
            << " | num_cores: " << config_.num_cores
            << " | connect: " << config_.connect
            << " | upstream_core: " << config_.upstream_core
            << " | num_downstream_cores: " << config_.num_downstream_cores
        );


        // Create downstream rings, or look them up if already created by a sibling processing core
        for (int ring_idx = 0; ring_idx < config_.num_downstream_cores; ring_idx++)
        {
            std::string downstream_ring_name = ring_name_str(config_.core_name, socket_id_, ring_idx);
            struct rte_ring* downstream_ring = rte_ring_lookup(downstream_ring_name.c_str());
            if (downstream_ring == NULL)
            {
                unsigned int downstream_ring_size = nearest_power_two(shared_buf_->get_num_buffers()*8);
                LOG4CXX_INFO(logger_, "Creating ring name "
                    << downstream_ring_name << " of size " << downstream_ring_size << " numa node: " << socket_id_
                );
                downstream_ring = rte_ring_create(
                    downstream_ring_name.c_str(), downstream_ring_size, socket_id_, RING_F_SC_DEQ
                );
                if (downstream_ring == NULL)
                {
                    throw std::runtime_error("Failed to create downstream ring " + downstream_ring_name
                        + ": " + rte_strerror(rte_errno));
                }
            }
            else
            {
                if (unlikely(debug_enabled_))
                {
                    LOG4CXX_DEBUG_LEVEL(2, logger_, "downstream ring with name "
                        << downstream_ring_name << " has already been created"
                    );
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
            if (unlikely(debug_enabled_))
            {
                LOG4CXX_DEBUG_LEVEL(2, logger_, "Creating frame processed ring name "
                    << clear_frames_ring_name << " of size " << clear_frames_ring_size << " numa node: " << socket_id_
                );
            }
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

    PacketProcessorCore::~PacketProcessorCore()
    {
        if (unlikely(debug_enabled_))
        {
            LOG4CXX_DEBUG_LEVEL(2, logger_, "PacketProcessorCore destructor");
        }

        // Stop the core polling loop so the run method terminates
        stop();
    }

    bool PacketProcessorCore::run(unsigned int lcore_id)
    {

        lcore_id_ = lcore_id;
        run_lcore_ = true;

        LOG4CXX_INFO(logger_, "Core " << lcore_id_ << " starting up");
        
        std::unordered_map<uint64_t, SuperFrameHeader*> frame_buffer_map_;

        uint64_t frame_outer_chunk_size = decoder_->get_frame_outer_chunk_size();

        // Set up structs needed for the various layers of packets
        struct RawFrameHeader *current_frame_header_;
        struct SuperFrameHeader *current_super_frame_buffer_;
        struct SuperFrameHeader *dropped_frame_buffer_;
        
        constexpr uint32_t MAX_BURST_SIZE = 128;
        struct rte_mbuf* pkt_burst[MAX_BURST_SIZE];
        struct rte_mbuf* pkt;
        
        struct rte_ether_hdr *pkt_ether_hdr;
        struct rte_udp_hdr *pkt_udp_hdr;
        PacketHeader* pkt_header;
        uint8_t *pkt_payload;
        

        // Cache offsets for packet header and payload
        const std::size_t super_frame_header_size = decoder_->get_super_frame_header_size();
        const std::size_t frame_header_size = decoder_->get_frame_header_size();
        const std::size_t udp_hdr_offset = sizeof(struct rte_ether_hdr) + 
                                          sizeof(struct rte_ipv4_hdr);
        const std::size_t pkt_hdr_offset = udp_hdr_offset + sizeof(struct rte_udp_hdr);
        const std::size_t pkt_payload_offset = decoder_->get_packet_payload_offset();
        
        // Variable set from the decoder based on packet size
        const std::size_t payload_size = decoder_->get_payload_size();
        const std::size_t packets_per_frame = decoder_->get_packets_per_frame();
        uint64_t frame_timeout_cycles = convert_ms_to_cycles(config_.frame_timeout_);
        uint64_t superframe_const = (decoder_->get_packets_per_frame() / frame_outer_chunk_size);

        uint64_t frames_per_second = 1;
        uint64_t last = rte_get_tsc_cycles();
        uint64_t cycles_per_sec = rte_get_tsc_hz();
        uint64_t cycles_working = 1;
        uint64_t start_frame_cycles = 1;
        uint64_t total_frame_cycles = 1;
        uint64_t maximum_frame_cycles = 1;
        uint64_t idle_loops = 0;
        uint64_t packets_per_second = 0;

        // Scratch buffer used when the clear_frames pool is empty; frames written here are dropped
        dropped_frame_buffer_ =
            reinterpret_cast<SuperFrameHeader *>(rte_malloc(NULL, decoder_->get_frame_buffer_size(), 0));

        while (likely(run_lcore_))
        {
            rte_prefetch0(packet_fwd_ring_); 
            
            // Batch dequeue - get multiple packets from the forwarding ring
            uint32_t nb_rx = rte_ring_dequeue_burst(packet_fwd_ring_, 
                                                    (void **)pkt_burst, 
                                                    MAX_BURST_SIZE, 
                                                    NULL);

            // Process the burst of packets if any were dequeued
            if (likely(nb_rx > 0))
            {
                start_frame_cycles = rte_get_tsc_cycles();
                
                // Process each packet in the burst
                for (uint32_t i = 0; i < nb_rx; i++)
                {
                    pkt = pkt_burst[i];
                    
                    // Prefetch next packet for better cache utilization
                    if (i + 1 < nb_rx)
                    {
                        rte_prefetch0(rte_pktmbuf_mtod(pkt_burst[i + 1], void *));
                    }
                    
                    total_packets_++;

                    // Get pointers to the ethernet, UDP, packet headers and payload
                    pkt_ether_hdr = rte_pktmbuf_mtod(pkt, rte_ether_hdr *);
                    pkt_udp_hdr = (struct rte_udp_hdr *)((uint8_t *)pkt_ether_hdr + udp_hdr_offset);
                    pkt_header = (PacketHeader *)((uint8_t *)pkt_ether_hdr + pkt_hdr_offset);
                    pkt_payload = (uint8_t *)((uint8_t *)pkt_ether_hdr + pkt_payload_offset);

                    // Get any frame/packet specific fields required for processing
                    uint16_t rx_port = rte_bswap16(pkt_udp_hdr->dst_port);

                    // Latch the first frame number on the first packet seen, offsetting by proc_idx_
                    // so that sibling processors each own a disjoint slice of the super-frame space
                    if (unlikely(first_frame_number_ == -1))
                    {
                        first_frame_number_ = decoder_->get_frame_number(pkt_header) - (proc_idx_ * decoder_->get_frame_outer_chunk_size());
                    }

                    uint64_t current_frame_number = decoder_->get_frame_number(pkt_header) - first_frame_number_;

                    uint64_t current_super_frame_number = (current_frame_number / frame_outer_chunk_size);

                    uint64_t current_frame_index = current_frame_number - (current_super_frame_number * decoder_->get_frame_outer_chunk_size());


                    uint32_t packet_offset = decoder_->get_packet_number(pkt_header) + ((current_frame_number % frame_outer_chunk_size) * superframe_const);

                    uint32_t packet_number = decoder_->get_packet_number(pkt_header);

                    // Check if the packet frame number matches the frame currently being captured
                    if (unlikely(current_frame_ != current_super_frame_number))
                    {

                        // If the packet frame number does not match the current frame, search the
                        // frame buffer map to see if it is currently being processed
                        auto result = frame_buffer_map_.find(current_super_frame_number);

                        // If a valid frame reference is found, swap to that frame
                        if (result != frame_buffer_map_.end())
                        {
                            current_super_frame_buffer_ = result->second;
                            // current_frame_ = current_frame_buffer_->current_frame_number;
                            current_frame_ = decoder_->get_super_frame_number(current_super_frame_buffer_);
                        }
                        else
                        {
                            // If a valid frame reference is not found for this packet, then obtain
                            // and map a new buffer for it
                            current_frame_ = current_super_frame_number;

                            // LOG4CXX_INFO(logger_, "Starting histogram: " << current_frame_number);

                            if (unlikely(rte_ring_dequeue(clear_frames_ring_, (void **) &current_super_frame_buffer_)) != 0)
                            {
                                current_super_frame_buffer_ = dropped_frame_buffer_;
                                dropped_frames_++;
                                LOG4CXX_WARN(logger_, "Using dropped_frame_buffer_");
                            }
                            else
                            {
                                frame_buffer_map_.insert(
                                    std::make_pair(current_super_frame_number, current_super_frame_buffer_)
                                );

                                // Clear any residual data from a previous acquisition that used this buffer
                                memset(current_super_frame_buffer_, 0, decoder_->get_frame_buffer_size());

                                decoder_->set_super_frame_number(current_super_frame_buffer_, current_super_frame_number);
                                decoder_->set_super_frame_start_time(
                                    current_super_frame_buffer_, rte_get_tsc_cycles()
                                );
                            }
                        }
                    }

                    if (unlikely(packet_number >= packets_per_frame))
                    {
                        LOG4CXX_WARN(logger_, "Dropping packet with out-of-range packet_number "
                            << packet_number << " (max " << packets_per_frame - 1 << ")"
                            << " frame " << current_frame_number
                        );
                        dropped_packets_++;
                        continue;
                    }

                    uint64_t packet_memory_offset = (current_frame_index * payload_size * packets_per_frame) + (packet_number * payload_size);

                    current_frame_header_ = decoder_->get_frame_header(current_super_frame_buffer_, current_frame_index);

                    rte_memcpy(
                        decoder_->get_image_data_start(current_super_frame_buffer_) + (current_frame_index * payload_size * packets_per_frame) +
                        (packet_number * payload_size), pkt_payload, payload_size
                    );

                    // LOG4CXX_TRACE(logger_,"Setting packet "<< packet_number << " as finished for frame " << current_frame_number);
                    // // Set the current packet as received in the frame header
                    if (decoder_->set_packet_received(current_frame_header_, packet_number))
                    {
                        if (decoder_->get_packets_received(current_frame_header_) == packets_per_frame)
                        {
                            // All packets for this sub-frame received; mark it complete in the super-frame
                            decoder_->set_super_frame_frames_received(current_super_frame_buffer_, current_frame_index);
                        }
                    }

                    // All sub-frames complete: forward the super-frame to the downstream ring
                    if (decoder_->get_super_frame_frames_received(current_super_frame_buffer_) == decoder_->get_frame_outer_chunk_size())
                    {
                        if (likely(current_super_frame_buffer_ != dropped_frame_buffer_))
                        {
                            rte_ring_enqueue(
                                downstream_rings_[
                                    (decoder_->get_super_frame_number(current_super_frame_buffer_) / frame_outer_chunk_size) %
                                    config_.num_downstream_cores
                                ], current_super_frame_buffer_
                            );

                            frame_buffer_map_.erase(current_frame_);
                            processed_frames_++;
                            frames_per_second++;
                        }
                        current_frame_ = -1;
                    }
                    
                    packets_per_second++;
                }
                
                // Batch enqueue all processed packets to be released
                rte_ring_enqueue_bulk(packet_release_ring_, (void **)pkt_burst, nb_rx, NULL);

                // Calculate status for the batch
                uint64_t cycles_spent = rte_get_tsc_cycles() - start_frame_cycles;
                total_frame_cycles += cycles_spent;
                cycles_working += cycles_spent;
                
                if (maximum_frame_cycles < cycles_spent)
                {
                    maximum_frame_cycles = cycles_spent;
                }
            }
            else
            {
                // No packets received, increment idle counter
                idle_loops++;
            }

            // Periodically check mapped frames to see if any have timed out and enqueue as
            // appropriate
            uint64_t now = rte_get_tsc_cycles();

            if (unlikely((now - last) >= (cycles_per_sec))) // Update status every second
            {
                // Update any monitoring variables every second
                processed_frames_hz_ = frames_per_second - 1;
                packets_hz_ = packets_per_second;
                mean_us_on_frame_ = (total_frame_cycles * 1000000) / (frames_per_second * cycles_per_sec);
                core_usage_ = (cycles_working * 255) / cycles_per_sec;

                maximum_us_on_frame_ = (maximum_frame_cycles * 1000000) / (cycles_per_sec);

                frame_buffer_size_ = frame_buffer_map_.size();

                idle_loops_ = idle_loops;

                

                // Reset any counters
                packets_per_second = 0;
                frames_per_second = 1;
                idle_loops = 0;
                total_frame_cycles = 1;
                cycles_working = 1;
                last = now;

                // Iterate through frames currently mapped
                for (auto it = frame_buffer_map_.begin(); it != frame_buffer_map_.end();)
                {
                    // Check if the time since the first packet received for the frame has
                    // exceeded the configured timeout
                    if (now - decoder_->get_super_frame_start_time(it->second) >= frame_timeout_cycles)
                    {
                        // set the frame metadata for dropped packets
                        
                        LOG4CXX_INFO(logger_, "Core " << lcore_id_
                            << " dropping super frame " << decoder_->get_super_frame_number(it->second)
                            << " with " << decoder_->get_super_frame_frames_received(it->second)
                            << " complete sub frames"
                            );

                        // Forward the incomplete super-frame downstream; there is always space
                        rte_ring_enqueue(
                            downstream_rings_[
                                (decoder_->get_super_frame_number(it->second) / frame_outer_chunk_size) %
                                config_.num_downstream_cores
                            ], it->second
                        );

                        incomplete_frames_++;
                        it = frame_buffer_map_.erase(it);
                    }
                    else
                    {
                        ++it;
                    }
                }

                last = now;
            }
            
        }
        rte_free(dropped_frame_buffer_);
        return true;
    }

    void PacketProcessorCore::stop(void)
    {
        if (run_lcore_)
        {
            LOG4CXX_INFO(logger_, "Core " << lcore_id_ << " stopping");
            run_lcore_ = false;
        }
        else
        {
            if (unlikely(debug_enabled_))
            {
                LOG4CXX_DEBUG_LEVEL(2, logger_, "Core " << lcore_id_ << " already stopped");
            }
        }

    }

    void PacketProcessorCore::status(OdinData::IpcMessage& status, const std::string& path)
    {
        if (unlikely(debug_enabled_))
        {
            LOG4CXX_DEBUG_LEVEL(2, logger_, "Status requested for packetprocessorcore_" << proc_idx_
                << " from the DPDK plugin");
        }

        std::string status_path = path + "/packetprocessorcore_" + std::to_string(proc_idx_) + "/";
        std::string ring_status = status_path + "upstream_rings/";
        std::string timing_status = status_path + "timing/";

        status.set_param(status_path + "dropped_frames", dropped_frames_);
        status.set_param(status_path + "dropped_packets", dropped_packets_);
        status.set_param(status_path + "frames_processed", processed_frames_);
        status.set_param(status_path + "frames_processed_per_second", processed_frames_hz_);
        status.set_param(status_path + "idle_loops", idle_loops_);
        status.set_param(status_path + "core_usage", (int)core_usage_);
        status.set_param(status_path + "frames_incomplete", incomplete_frames_);
        status.set_param(status_path + "packets_total", total_packets_);
        status.set_param(status_path + "frame_buffer_size", frame_buffer_size_);

        status.set_param(timing_status + "mean_frame_us", mean_us_on_frame_);
        status.set_param(timing_status + "max_frame_us", maximum_us_on_frame_);

        status.set_param(ring_status + ring_name_str(config_.upstream_core, socket_id_, proc_idx_) + "_count", rte_ring_count(packet_fwd_ring_));
        status.set_param(ring_status + ring_name_str(config_.upstream_core, socket_id_, proc_idx_) + "_size", rte_ring_get_size(packet_fwd_ring_));
    }

    bool PacketProcessorCore::connect(void)
    {
        std::string upstream_ring_name = ring_name_str(config_.upstream_core, socket_id_, proc_idx_);
        struct rte_ring* upstream_ring = rte_ring_lookup(upstream_ring_name.c_str());
        if (upstream_ring == NULL)
        {
            LOG4CXX_ERROR(logger_, config_.core_name << " : " << proc_idx_ << " Failed to connect to upstream ring: " << upstream_ring_name);
            return false;
        }
        packet_fwd_ring_ = upstream_ring;

        std::string packet_release_ring_name = ring_name_pkt_release(socket_id_);
        packet_release_ring_ = rte_ring_lookup(packet_release_ring_name.c_str());
        if (packet_release_ring_ == NULL)
        {
            LOG4CXX_ERROR(logger_, config_.core_name << " : " << proc_idx_ << " Failed to connect to packet release ring: " << packet_release_ring_name);
            return false;
        }

        LOG4CXX_INFO(logger_, config_.core_name << " : " << proc_idx_ << " Connected to upstream resources successfully!");

        return true;
    }

    void PacketProcessorCore::configure(OdinData::IpcMessage& config)
    {
        LOG4CXX_INFO(logger_, config_.core_name << " : " << proc_idx_ << " Got update config.");

        if (config.get_param("proc_enable", false))
        {
            first_frame_number_ = -1;
            LOG4CXX_INFO(logger_, config_.core_name << " : " << proc_idx_ << " Reset frame latch");
        }

    }

    std::vector<std::pair<std::string, int>> PacketProcessorCore::requestCommands()
    {
        return {{"start_capture", DEFAULT_COMMAND_PRIORITY + 1}};
    }

    void PacketProcessorCore::execute(const std::string& command, OdinData::IpcMessage& reply)
    {
        if (command == "start_capture")
        {
            start_capture();
        }
        else
        {
            reply.set_nack("PacketProcessorCore: unknown command: " + command);
        }
    }

    void PacketProcessorCore::start_capture()
    {
        first_frame_number_ = -1;
        LOG4CXX_INFO(logger_, config_.core_name << " : " << proc_idx_ << " start_capture: first_frame_number_ reset to -1");
    }

    DPDKREGISTER(DpdkWorkerCore, PacketProcessorCore, "PacketProcessorCore");
}