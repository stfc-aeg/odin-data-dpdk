
#include "PacketProcessorCore.h"

#include <iostream>
#include <unordered_map>

#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_udp.h>
#include <rte_malloc.h>
#include "DpdkUtils.h"

namespace FrameProcessor
{
    PacketProcessorCore::PacketProcessorCore(
        int proc_idx, int socket_id, DpdkWorkCoreReferences dpdkWorkCoreReferences
    ) :
        DpdkWorkerCore(socket_id),
        proc_idx_(proc_idx),
        decoder_(dpdkWorkCoreReferences.decoder),
        shared_buf_(dpdkWorkCoreReferences.shared_buf),
        dropped_frames_(0),
        dropped_packets_(0),
        current_frame_(-1),
        incomplete_frames_(0),
        complete_frames_(0),
        frames_complete_hz_(0),
        logger_(Logger::getLogger("FP.PacketProcCore"))
    {

        // Resolve configuration parameters for this core from the config object passed as an
        // argument, and the current port ID
        config_.resolve(dpdkWorkCoreReferences.core_config);

        LOG4CXX_INFO(logger_, "FP.PacketProcCore " << proc_idx_ << " Created with config:"
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

    PacketProcessorCore::~PacketProcessorCore()
    {
        LOG4CXX_DEBUG_LEVEL(2, logger_, "PacketProcessorCore destructor");

        // Stop the core polling loop so the run method terminates
        stop();
    }

    bool PacketProcessorCore::run(unsigned int lcore_id)
    {

        lcore_id_ = lcore_id;
        run_lcore_ = true;

        LOG4CXX_INFO(logger_, "Core " << lcore_id_ << " starting up");
        
        std::unordered_map<uint64_t, RawFrameHeader*> frame_buffer_map_;

        // Set up structs needed for the various layers of packets
        struct RawFrameHeader *current_frame_buffer_;
        struct RawFrameHeader *dropped_frame_buffer_;
        struct rte_mbuf* pkt;
        struct rte_ether_hdr *pkt_ether_hdr;
        struct rte_udp_hdr *pkt_udp_hdr;
        PacketHeader* pkt_header;
        uint8_t *pkt_payload;

        // Calculate offsets to the various layers of incoming packets
        const std::size_t udp_hdr_offset =
            sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr);
        const std::size_t pkt_hdr_offset = udp_hdr_offset + sizeof(struct rte_udp_hdr);
        const std::size_t pkt_payload_offset = pkt_hdr_offset + decoder_->get_packet_header_size();
        const std::size_t frame_header_size = decoder_->get_frame_header_size();
        
        // Variable set from the decoder based on packet size
        const std::size_t payload_size = decoder_->get_payload_size();
        const std::size_t packets_per_frame = decoder_->get_packets_per_frame();

        // Initialise counters and timers
        uint64_t ticks_per_sec = rte_get_tsc_hz();
        uint64_t last = rte_get_tsc_cycles();
        uint64_t loops_per_sec = 0;
        uint64_t frame_hz_counter = 0;
        uint64_t start = rte_get_tsc_cycles();
        uint64_t start_of_compression = rte_get_tsc_cycles();
        uint64_t frame_timeout_cycles = convert_ms_to_cycles(config_.frame_timeout_);

        // malloc a memory location for this core to use as it's dropped frame buffer
        dropped_frame_buffer_ = 
            reinterpret_cast<RawFrameHeader *>(rte_malloc(NULL, decoder_->get_frame_buffer_size(), 0));

        while (likely(run_lcore_))
        {
            // Get a packet from the forwarding ring if available
            int rc = rte_ring_dequeue(packet_fwd_ring_, (void **)&pkt);

            // If a packet was deqeued process it
            if (likely(rc == 0))
            {

                // Get pointers to the ethernet, UDP, packet headers and payload
                pkt_ether_hdr = rte_pktmbuf_mtod(pkt, rte_ether_hdr *);
                pkt_udp_hdr = (struct rte_udp_hdr *)((uint8_t *)pkt_ether_hdr + udp_hdr_offset);
                pkt_header = (PacketHeader *)((uint8_t *)pkt_ether_hdr + pkt_hdr_offset);
                pkt_payload = (uint8_t *)((uint8_t *)pkt_ether_hdr + pkt_payload_offset);

                // Get any frame/packet specific fields required for processing
                uint16_t rx_port = rte_bswap16(pkt_udp_hdr->dst_port);
                uint64_t frame_number = decoder_->get_frame_number(pkt_header);
                uint32_t packet_number = decoder_->get_packet_number(pkt_header);

                LOG4CXX_DEBUG_LEVEL(3, logger_, "Proc core idx " << proc_idx_
                    << " on lcore " << lcore_id_
                    << " got packet on port: " << rx_port
                    << " frame: " << frame_number << " packet " << packet_number
                );

                // Check if the packet frame number matches the frame currently being captured
                if (unlikely(current_frame_ != frame_number))
                {

                    // If the packet frame number does not match the current frame, search the
                    // frame buffer map to see if it is currently being processed
                    auto result = frame_buffer_map_.find(frame_number);

                    // If a valid frame reference is found, swap to that frame
                    if (result != frame_buffer_map_.end())
                    {
                        current_frame_buffer_ = result->second;
                        // current_frame_ = current_frame_buffer_->frame_number;
                        current_frame_ = decoder_->get_frame_number(current_frame_buffer_);
                    }
                    else
                    {
                        // If a valid frame reference is not found for this packet, then obtain
                        // and map a new buffer for it
                        current_frame_ = frame_number;

                        // Try and dequeue a shared buffer from the frame processed ring for the
                        // frame to be built into
                        if (unlikely(rte_ring_dequeue(
                            clear_frames_ring_, (void **) &current_frame_buffer_
                        )) != 0)
                        {
                            // If a memory location cannot be found start dropping this frame
                            current_frame_buffer_ = dropped_frame_buffer_;
                            dropped_frames_++;
                            LOG4CXX_WARN(logger_,
                                 "dropping frame: " << current_frame_);
                        }
                        else
                        {
                            // If a buffer was successfully obtained, then add it to the buffer map
                            frame_buffer_map_.insert(
                                std::make_pair(frame_number, current_frame_buffer_)
                            );

                            // Zero out the frame header to clear old data
                            memset(current_frame_buffer_, 0, frame_header_size);

                            // Set the frame number and start time in the header
                            decoder_->set_frame_number(current_frame_buffer_, frame_number);
                            decoder_->set_frame_start_time(
                                current_frame_buffer_, rte_get_tsc_cycles()
                            );
                        }
                    }
                }

                // Copy the packet payload into the appropriate location in the frame buffer
                rte_memcpy(
                    reinterpret_cast<char *>(current_frame_buffer_) + frame_header_size +
                    (packet_number * payload_size), pkt_payload, payload_size
                );

                // Set the current packet as received in the frame header
                if (!decoder_->set_packet_received(current_frame_buffer_, packet_number))
                {
                    // TODO handle illegal packet number here - maybe too late since already
                    // copied into buffer based on packet number? Swap order with rte_memcpy call
                    // above??
                }

                // Look to check the SOF & EOF markers
                // check to see if the frame is complete

                if (decoder_->get_packets_received(current_frame_buffer_) == packets_per_frame)
                {
                    // The frame is complete, so enqueue the frame reference for the
                    // FrameBuilderCore to pick up. Check if the current frame is 'dropped' and
                    // don't enqueue if that is the case

                    if (likely(current_frame_buffer_ != dropped_frame_buffer_))
                    {
                        rte_ring_enqueue(
                            downstream_rings_[
                                decoder_->get_frame_number(current_frame_buffer_) %
                                config_.num_downstream_cores
                            ], current_frame_buffer_
                        );

                        // Remove the frame reference from the unordered map
                        frame_buffer_map_.erase(current_frame_);
                        
                        complete_frames_++;
                        frame_hz_counter++;
                    }
                    current_frame_ = -1;
                }

                // Enqueue the packet to be released as it's been copied onwards
                rte_ring_enqueue(packet_release_ring_, (void *)pkt);
            }

            // Periodically check mapped frames to see if any have timed out and enqueue as
            // appropriate
            uint64_t now = rte_get_tsc_cycles();

            if (unlikely((now - last) >= (ticks_per_sec)))
            {
                frames_complete_hz_ = frame_hz_counter;
                frame_hz_counter = 0;

                // Iterate through frames currently mapped
                for (auto it = frame_buffer_map_.begin(); it != frame_buffer_map_.end();)
                {
                    // Check if the time since the first packet received for the frame has
                    // exceeded the configured timeout
                    if (now - decoder_->get_frame_start_time(it->second) >= frame_timeout_cycles)
                    {
                        // set the frame metadata for dropped packets
                        
                        LOG4CXX_INFO (logger_, "Core " << lcore_id_
                            << " dropping frame " << decoder_->get_frame_number(it->second)
                            << " with " << decoder_->get_packets_dropped(it->second)
                            << " missing packets"
                            );

                        // Increment the total dropped packets counter with the number of packets
                        // dropped from the frame
                        dropped_packets_ += decoder_->get_packets_dropped(it->second);

                        // Enqueue the frame reference for the FrameBuilderCore to pick up
                        // there will always be space on this ring, so no retry checks are needed

                        rte_ring_enqueue(
                            downstream_rings_[
                                decoder_->get_frame_number(it->second) %
                                config_.num_downstream_cores
                            ], it->second
                        );

                        // Increment the counter for incomplete frames
                        incomplete_frames_++;

                        // Remove the frame from the frame buffer map, updating the map iterator
                        it = frame_buffer_map_.erase(it);
                    }
                    else
                    {
                        // Increment the iterator to check the next element in the frame buffer map
                        ++it;
                    }
                }

                // Update the timestamp for checking for timed-out frames
                last = now;
                loops_per_sec = 0;
            }
            loops_per_sec++;
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
            LOG4CXX_DEBUG_LEVEL(2, logger_, "Core " << lcore_id_ << " already stopped");
        }

    }

    void PacketProcessorCore::status(OdinData::IpcMessage& status, const std::string& path)
    {
        LOG4CXX_DEBUG(logger_, "Status requested for packetprocessorcore_" << proc_idx_
            << " from the DPDK plugin");

        std::string status_path = path + "/packetprocessorcore_" + std::to_string(proc_idx_) + "/";

        status.set_param(status_path + "dropped_frames", dropped_frames_);

        status.set_param(status_path + "dropped_packets", dropped_packets_);

        status.set_param(status_path + "current_frame", current_frame_);

        status.set_param(status_path + "frames_incomplete", incomplete_frames_);

        status.set_param(status_path + "frames_complete_total", complete_frames_);

        status.set_param(status_path + "frames_complete_hz", frames_complete_hz_);
    }

    bool PacketProcessorCore::connect(void)
    {

        // connect to the ring for incoming packets
        std::string upstream_ring_name = ring_name_str(config_.upstream_core, socket_id_, proc_idx_);
        struct rte_ring* upstream_ring = rte_ring_lookup(upstream_ring_name.c_str());
        if (upstream_ring == NULL)
        {
            // this needs to error out as there should always be upstream resources at this point
            LOG4CXX_INFO(logger_, config_.core_name << " : " << proc_idx_ << " Failed to Connect to upstream resources!: " << upstream_ring_name );
            return false;
        }
        else
        {
            packet_fwd_ring_ = upstream_ring;
            LOG4CXX_DEBUG_LEVEL(2, logger_, "Frame ready ring with name "
                << upstream_ring_name << " has already been created"
            );  
        }

        // connect to the ring for dumping old packets
        std::string packet_release_ring_name = ring_name_pkt_release(socket_id_);
        packet_release_ring_ = rte_ring_lookup(packet_release_ring_name.c_str());
        if (packet_release_ring_ == NULL)
        {
            // this needs to error out as there should always be upstream resources at this point
            LOG4CXX_INFO(logger_, config_.core_name << " : " << proc_idx_ << " Failed to Connect to upstream resources!" << packet_release_ring_name );
            return false;
        }
        else
        {
            LOG4CXX_DEBUG_LEVEL(2, logger_, "Packet release ring with name "
                << packet_release_ring_name << " has already been created"
            );
        }


        LOG4CXX_INFO(logger_, config_.core_name << " : " << proc_idx_ << " Connected to upstream resources successfully!");

        return true;
    }

    DPDKREGISTER(DpdkWorkerCore, PacketProcessorCore, "PacketProcessorCore");
}