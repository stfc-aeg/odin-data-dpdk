
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
        first_frame_number_(-1),
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
        
        std::unordered_map<uint64_t, SuperFrameHeader*> frame_buffer_map_;

        uint64_t frame_outer_chunk_size = decoder_->get_frame_outer_chunk_size();

        // Set up structs needed for the various layers of packets
        struct RawFrameHeader *current_frame_header_;
        struct SuperFrameHeader *current_super_frame_buffer_;
        struct SuperFrameHeader *dropped_frame_buffer_;
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
        const std::size_t super_frame_header_size = decoder_->get_super_frame_header_size();
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

        uint64_t superframe_const = (decoder_->get_packets_per_frame() / frame_outer_chunk_size);

        // malloc a memory location for this core to use as it's dropped frame buffer
        dropped_frame_buffer_ = 
            reinterpret_cast<SuperFrameHeader *>(rte_malloc(NULL, decoder_->get_frame_buffer_size(), 0));

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

                // This statement allows the code to reset the "starting" frame number in code
                // When first_frame_number_ is set to -1 the next first packet of a frame will be use
                // to create a variable to offset the frame number by, allow for frames to be
                // distributed as it the first frame has a frame number of 0

                if(unlikely(first_frame_number_ == -1))
                {
                    first_frame_number_ = decoder_->get_frame_number(pkt_header) - (proc_idx_ * decoder_->get_frame_outer_chunk_size());

                    LOG4CXX_INFO(logger_, config_.core_name << " : " << proc_idx_ << " Updated frame latch to: " << first_frame_number_ 
                        << " Frame number will be: " << (decoder_->get_frame_number(pkt_header) - first_frame_number_) / frame_outer_chunk_size);
                }

                uint64_t current_frame_number = decoder_->get_frame_number(pkt_header) - first_frame_number_;

                uint64_t current_super_frame_number = (current_frame_number / frame_outer_chunk_size);

                uint64_t current_frame_index = current_frame_number - (current_super_frame_number * 1000);


                // LOG4CXX_INFO(logger_, "Core " << lcore_id_
                //             << " current_frame_number: " << current_frame_number
                //             << " current_super_frame_number: " << current_super_frame_number
                //             << " current_frame_index: " << current_frame_index
                //             );

                // Get the packet offset for the bulk frame

                // packet number = pkt_num_from_packet + (packets per frame )
                uint32_t packet_number = decoder_->get_packet_number(pkt_header) + ((current_frame_number % frame_outer_chunk_size) * superframe_const);

                //uint32_t packet_number = decoder_->get_packet_number(pkt_header) + (current_frame_number * packets_per_frame);

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

                        // Try and dequeue a shared buffer from the frame processed ring for the
                        // frame to be built into
                        if (unlikely(rte_ring_dequeue(
                            clear_frames_ring_, (void **) &current_super_frame_buffer_
                        )) != 0)
                        {
                            // If a memory location cannot be found start dropping this frame
                            current_super_frame_buffer_ = dropped_frame_buffer_;
                            dropped_frames_++;
                            LOG4CXX_WARN(logger_,
                                 "dropping frame: " << current_frame_);
                        }
                        else
                        {
                            // If a buffer was successfully obtained, then add it to the buffer map
                            frame_buffer_map_.insert(
                                std::make_pair(current_super_frame_number, current_super_frame_buffer_)
                            );

                            // Zero out the frame header to clear old data
                            memset(current_super_frame_buffer_, 0, decoder_->get_frame_buffer_size());

                            // Set the frame number and start time in the header
                            decoder_->set_super_frame_number(current_super_frame_buffer_, current_super_frame_number);
                            decoder_->set_super_frame_start_time(
                                current_super_frame_buffer_, rte_get_tsc_cycles()
                            );
                        }
                    }
                }

                current_frame_header_ = decoder_->get_frame_header(current_super_frame_buffer_, current_frame_index);
                
                // Copy the packet payload into the appropriate location in the frame buffer
                rte_memcpy(
                    decoder_->get_image_data_start(current_super_frame_buffer_) + (current_frame_index * payload_size * packets_per_frame) + 
                    (packet_number * payload_size), pkt_payload, payload_size
                );

                //LOG4CXX_INFO(logger_,"Setting packet "<< packet_number << " as finished for frame " << current_frame_number);
                // // Set the current packet as received in the frame header
                if (decoder_->set_packet_received(current_frame_header_, packet_number))
                {
                    //LOG4CXX_INFO(logger_,"Checking frame " << current_frame_number << " with " << decoder_->get_packets_received(decoder_->get_frame_header(current_super_frame_buffer_, current_frame_number)) << " Packets");
                    // Check to see if that frames has been completed in the superframe
                    if(decoder_->get_packets_received(current_frame_header_) == packets_per_frame)
                    {
                        
                        // All packets for this sub-frame have been captured, mark it as complete
                        if (decoder_->set_super_frame_frames_recieved(current_super_frame_buffer_, current_frame_number))
                        {
                            // TODO handle illegal frame number here
                        }
                    }
                }
                else
                {
                    // TODO handle illegal f number here - maybe too late since already
                    // copied into buffer based on packet number? Swap order with rte_memcpy call
                    // above??
                }

                // Look to check the SOF & EOF markers
                // check to see if the frame is complete

                if (decoder_->get_super_frame_frames_recieved(current_super_frame_buffer_) == decoder_->get_frame_outer_chunk_size())
                {
                    // The frame is complete, so enqueue the frame reference for the
                    // FrameBuilderCore to pick up. Check if the current frame is 'dropped' and
                    // don't enqueue if that is the case

                    if (likely(current_super_frame_buffer_ != dropped_frame_buffer_))
                    {
                        rte_ring_enqueue(
                            downstream_rings_[
                                (decoder_->get_super_frame_number(current_super_frame_buffer_) / frame_outer_chunk_size) % 
                                config_.num_downstream_cores
                            ], current_super_frame_buffer_
                        );

                        // Remove the frame reference from the unordered map
                        frame_buffer_map_.erase(current_frame_);
                        
                        complete_frames_++;
                        frame_hz_counter++;

                        //LOG4CXX_INFO(logger_, config_.core_name << " : " << proc_idx_ << " Capture all packets for frame: " << current_frame_);
                    }
                    current_frame_ = -1;
                }

                // Enqueue the packet to be released as it's been copied onwards
                rte_ring_enqueue(packet_release_ring_, (void *)pkt);
            }

            // Periodically check mapped frames to see if any have timed out and enqueue as
            // appropriate
            uint64_t now = rte_get_tsc_cycles();

            if (unlikely((now - last) >= (ticks_per_sec * 2)))
            {
                frames_complete_hz_ = frame_hz_counter;
                frame_hz_counter = 0;

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
                            << " with " << decoder_->get_super_frame_frames_recieved(it->second)
                            << " complete sub frames"
                            );

                        // Increment the total dropped packets counter with the number of packets
                        // dropped from the frame
                        //dropped_packets_ += decoder_->get_packets_dropped(it->second);

                        // Enqueue the frame reference for the FrameBuilderCore to pick up
                        // there will always be space on this ring, so no retry checks are needed

                        rte_ring_enqueue(
                            downstream_rings_[
                                (decoder_->get_super_frame_number(it->second) / frame_outer_chunk_size) %
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


    void PacketProcessorCore::configure(OdinData::IpcMessage& config)
    {
        // Update the config based from the passed IPCmessage

        LOG4CXX_INFO(logger_, config_.core_name << " : " << proc_idx_ << " Got update config.");

        if (config.get_param("proc_enable", false))
        {
            first_frame_number_ = -1;
            LOG4CXX_INFO(logger_, config_.core_name << " : " << proc_idx_ << "Reset frame latch");
        }

    }

    DPDKREGISTER(DpdkWorkerCore, PacketProcessorCore, "PacketProcessorCore");
}