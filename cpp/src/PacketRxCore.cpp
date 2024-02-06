#include <algorithm>
#include <sstream>
#include "PacketRxCore.h"
#include "DpdkUtils.h"

namespace FrameProcessor
{
    PacketRxCore::PacketRxCore(
        int proc_idx, int socket_id, DpdkWorkCoreReferences dpdkWorkCoreReferences
    ) :
        DpdkWorkerCore(socket_id),
        port_id_(dpdkWorkCoreReferences.port_id),
        proc_idx_(proc_idx),
        decoder_(dpdkWorkCoreReferences.decoder),
        logger_(Logger::getLogger("FP.PacketRxCore"))
    {

        // Resolve configuration parameters for this core from the config object passed as an
        // argument, and the current port ID
        config_.resolve(dpdkWorkCoreReferences.core_config);

        LOG4CXX_INFO(logger_, "FP.PacketRxCore " << proc_idx_ << " Created with config:"
            << " | core_name" << config_.core_name
            << " | num_cores: " << config_.num_cores
            << " | connect: " << config_.connect
            << " | num_downsteam_cores: " << config_.num_downstream_cores
        );


        // Resolve the device MAC address for this port, to allow ARP requests to be responded to
        int rc = rte_eth_macaddr_get(port_id_, &dev_eth_addr_);
        if (rc != 0)
        {
            LOG4CXX_ERROR(logger_, "Error getting MAC address for device on port " << port_id_
                << " : " << rte_strerror(rc)
            );
            // TODO - raise exception here?
        }

        // DPDK does not implement an IP stack, so cannot resolve any existing IP address assigned
        // by the kernel to the ethernet device. The IP address, which is also required to respond
        // to ARP requests, must be provided from configuration
        if (inet_pton(AF_INET, config_.device_ip_.c_str(), &dev_ip_addr_) < 1)
        {
            LOG4CXX_ERROR(logger_, "Error resolving device IP address for port " << port_id_
                << " from value" << config_.device_ip_
            );
            // TODO - raise exception here?
        }

        LOG4CXX_DEBUG_LEVEL(2, logger_, "Ethernet device on port " << port_id_
            << " has MAC address " << mac_addr_str(dev_eth_addr_)
            << " IP address " << ip_addr_str(dev_ip_addr_)
        );

        unsigned int ring_size;
        std::string ring_name;

        // Create packet forwarding rings for each of the packet processing cores with the ring
        // size rounded up to the next power of two
        ring_size = nearest_power_two(config_.fwd_ring_size_);
        for (int core_idx = 0; core_idx < config_.num_downstream_cores; core_idx++)
        {
            ring_name = ring_name_str(config_.core_name, socket_id_, core_idx);
            LOG4CXX_INFO(logger_, "Creating packet forward ring name "
                << ring_name << " of size " << ring_size
            );
            struct rte_ring *fwd_ring = rte_ring_create(
                ring_name.c_str(), ring_size, socket_id_, RING_F_SP_ENQ | RING_F_SC_DEQ
            );
            if (fwd_ring == NULL)
            {
                LOG4CXX_ERROR(logger_, "Error creating packet forward ring " << ring_name
                    << " : " << rte_strerror(rte_errno)
                );
                // TODO - raise exception here?
            }
            packet_forward_rings_.push_back(fwd_ring);
        }

        // Create the packet release ring with the ring size rounded up to the next power of two
        ring_name = ring_name_pkt_release(socket_id_);
        ring_size = nearest_power_two(config_.release_ring_size_);
        LOG4CXX_DEBUG_LEVEL(2, logger_, "Creating packet release ring name "
            << ring_name << " of size " << ring_size
        );
        packet_release_ring_ = rte_ring_create(ring_name.c_str(), ring_size, socket_id_, 0);
        if (packet_release_ring_ == NULL)
        {
            LOG4CXX_ERROR(logger_, "Error creating packet release ring " << ring_name
                << " : " << rte_strerror(rte_errno)
            );
            // TODO - raise exception here?
        }

        // Check that at least one RX port has been defined
        if (config_.rx_ports_.size() == 0)
        {
            LOG4CXX_ERROR(logger_, "No RX ports defined");
            // TODO - raise exception here?
        }
        else
        {
            LOG4CXX_INFO(logger_, "Receiving packets on "
                << config_.rx_ports_.size() << " ports: "
                << port_list_str(config_.rx_ports_)
            );
        }

        LOG4CXX_INFO(logger_, "PacketRxCore " << proc_idx_ << " Created");
    }

    PacketRxCore::~PacketRxCore()
    {
        LOG4CXX_DEBUG_LEVEL(2, logger_, "PacketRxCore destructor");

        // Stop the core polling loop so the run method terminates
        stop();

        // Free the packet forwarding rings
        for (auto& fwd_ring: packet_forward_rings_)
        {
            rte_ring_free(fwd_ring);
        }
        packet_forward_rings_.clear();
        std::vector<struct rte_ring *>(packet_forward_rings_).swap(packet_forward_rings_);

        // Free the packet release ring
        rte_ring_free(packet_release_ring_);

    }

    bool PacketRxCore::run(unsigned int lcore_id)
    {
        lcore_id_ = lcore_id;
        run_lcore_ = true;

        LOG4CXX_INFO(logger_, "PacketRxCore " << lcore_id_ << " starting up");

        struct rte_mbuf *pkt_bufs[config_.rx_burst_size_];
        struct rte_mbuf *pkt;
        struct rte_mbuf *release_pkt[config_.rx_burst_size_];
        struct rte_ether_hdr *pkt_ether_hdr;
        struct rte_arp_hdr *pkt_arp_hdr;
        struct rte_ipv4_hdr *pkt_ipv4_hdr;
        struct rte_icmp_hdr *pkt_icmp_hdr;
        struct rte_udp_hdr *pkt_udp_hdr;

        uint64_t first = rte_get_tsc_cycles();
        uint64_t ticks_per_sec = rte_get_tsc_hz();

        uint16_t num_replies = 0;

        uint64_t packet_counter_ = 0;

        bool pkt_tx_reply = false;
        bool pkt_forwarded = false;

        while (likely(run_lcore_))
        {

            uint16_t num_rx_pkts = rte_eth_rx_burst(
                port_id_, config_.rx_queue_id_, pkt_bufs, config_.rx_burst_size_
            );

            for (uint16_t idx = 0; idx < num_rx_pkts; idx++)
            {
                pkt_tx_reply = false;
                pkt_forwarded = false;

                if (likely(idx < num_rx_pkts - 1))
                {
                    rte_prefetch0(rte_pktmbuf_mtod(pkt_bufs[idx + 1], void *));
                }
                pkt = pkt_bufs[idx];
                pkt_ether_hdr = rte_pktmbuf_mtod(pkt, struct rte_ether_hdr *);

                switch(rte_bswap16(pkt_ether_hdr->ether_type))
                {
                    case RTE_ETHER_TYPE_ARP:

                        pkt_arp_hdr = (struct rte_arp_hdr *)(
                            (uint8_t *)pkt_ether_hdr + sizeof(struct rte_ether_hdr)
                        );

                        pkt_tx_reply = handle_arp_request(&pkt_ether_hdr, &pkt_arp_hdr);
                        break;

                    case RTE_ETHER_TYPE_IPV4:

                        pkt_ipv4_hdr = (struct rte_ipv4_hdr *)(
                            (uint8_t *)pkt_ether_hdr + sizeof(struct rte_ether_hdr)
                        );

                        switch(pkt_ipv4_hdr->next_proto_id)
                        {
                            case IPPROTO_ICMP:

                                pkt_icmp_hdr = (struct rte_icmp_hdr *)(
                                    (uint8_t *)pkt_ipv4_hdr + sizeof(struct rte_ipv4_hdr)
                                );

                                pkt_tx_reply = handle_icmp_request(
                                    &pkt_ether_hdr, &pkt_ipv4_hdr, &pkt_icmp_hdr
                                );
                                break;

                            case IPPROTO_UDP:

                                pkt_udp_hdr = (struct rte_udp_hdr *)(
                                    (uint8_t *)pkt_ipv4_hdr + sizeof(struct rte_ipv4_hdr)
                                );

                                pkt_forwarded = handle_udp_packet(
                                    &pkt, &pkt_ether_hdr, &pkt_ipv4_hdr, &pkt_udp_hdr
                                );

                                packet_counter_++;
                                
                                if(unlikely(packet_counter_ == 1))
                                {
                                    first = rte_get_tsc_cycles();
                                    LOG4CXX_INFO(logger_, config_.core_name << " : " << proc_idx_ << " First packet: " <<  first );
                                }                             //15000000
                                if (unlikely(packet_counter_ == 15000000))
                                {
                                    LOG4CXX_INFO(logger_, config_.core_name << " : " << proc_idx_ << " Time to recieve data: " << (float) (rte_get_tsc_cycles() - first) / (float) ticks_per_sec );
                                }

                                
                                break;

                            default:
                                break;

                        } // switch(pkt_ipv4_hdr->next_proto_id)
                        break;

                    default:
                        break;

                } // switch(rte_bswap16(pkt_ether_hdr->ether_type))

                // If a handler wants to send a reply to the packet, add it to the buffer
                // and increment the number of replies. If the packet has been forwarded by a
                // handler (e.g. valid UDP packets) do nothing, otherwise free the packet mbuf
                if (pkt_tx_reply)
                {
                    pkt_bufs[num_replies++] = pkt;
                }
                else if (pkt_forwarded)
                {
                    // Do nothing with the packet - handler has forwarded it
                }
                else
                {
                    rte_pktmbuf_free(pkt);
                }
            } // for (uint16_t idx = 0; idx < num_rx_pkts; idx++)

            // If any replies have been generated, queue them for TX
            if (num_replies > 0)
            {
                uint16_t num_tx_pkts = rte_eth_tx_burst(
                    port_id_, config_.tx_queue_id_, pkt_bufs, num_replies
                );

                if (unlikely(num_tx_pkts < num_replies))
                {
                    uint32_t retry = 0;
                    while ((num_tx_pkts < num_replies) && (retry++ < config_.max_packet_tx_retries_))
                    {
                        rte_delay_us(1);
                        num_tx_pkts += rte_eth_tx_burst(
                            port_id_, config_.tx_queue_id_, &pkt_bufs[num_tx_pkts],
                            num_replies - num_tx_pkts
                        );
                    }
                }

                if (unlikely(num_tx_pkts < num_replies))
                {
                    do {
                        rte_pktmbuf_free(pkt_bufs[num_tx_pkts]);
                    } while (++num_tx_pkts < num_replies);
                }

                num_replies = 0;

            }

            // Free packets fed back on the release ring from downstream cores
            if (rte_ring_dequeue_bulk(packet_release_ring_, (void **)&release_pkt, config_.rx_burst_size_, NULL) > 0)
            {
                rte_pktmbuf_free_bulk((struct rte_mbuf **)&release_pkt, config_.rx_burst_size_);
            }
        }

        return true;
    }

    void PacketRxCore::stop(void)
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

    /**
     * @brief Handle an ARP request packet.
     *
     * Check if the target IP address in the ARP request matches the device's IP address. If so,
     * build a reply and set the appropriate fields.
     *
     * @param [in] pkt_ether_hdr A pointer to the Ethernet header of the packet.
     * @param [in] pkt_arp_hdr A pointer to the ARP header of the packet.
     * @return true if the packet is handled and a reply is sent, false otherwise.
     */
    bool PacketRxCore::handle_arp_request(
        struct rte_ether_hdr **pkt_ether_hdr, struct rte_arp_hdr **pkt_arp_hdr
    )
    {
        bool tx_reply = false;

        if ((*pkt_arp_hdr)->arp_opcode == rte_cpu_to_be_16(RTE_ARP_OP_REQUEST))
        {
            LOG4CXX_DEBUG_LEVEL(3, logger_, "RX ARP REQUEST: " << lcore_id_
                        << " MAC src: " << mac_addr_str((*pkt_ether_hdr)->src_addr)
                        << " dst: " << mac_addr_str((*pkt_ether_hdr)->dst_addr)
                        << " IP src: " << ip_addr_str((*pkt_arp_hdr)->arp_data.arp_sip)
                        << " tgt: " << ip_addr_str((*pkt_arp_hdr)->arp_data.arp_tip)
            );

            // If the target IP address in the ARP request matches this device, build a reply
            if ((*pkt_arp_hdr)->arp_data.arp_tip == dev_ip_addr_)
            {
                tx_reply = true;

                // Set ARP opcode to reply
                (*pkt_arp_hdr)->arp_opcode = rte_cpu_to_be_16(RTE_ARP_OP_REPLY);

                // Switch source and destination data in reply, setting device MAC and IP
                rte_ether_addr_copy(&((*pkt_ether_hdr)->src_addr), &((*pkt_ether_hdr)->dst_addr));
                rte_ether_addr_copy(&dev_eth_addr_, &((*pkt_ether_hdr)->src_addr));

                rte_ether_addr_copy(&((*pkt_arp_hdr)->arp_data.arp_sha),
                    &((*pkt_arp_hdr)->arp_data.arp_tha));
                rte_ether_addr_copy(&dev_eth_addr_, &((*pkt_arp_hdr)->arp_data.arp_sha));

                ((*pkt_arp_hdr)->arp_data.arp_tip) = ((*pkt_arp_hdr)->arp_data.arp_sip);
                ((*pkt_arp_hdr)->arp_data.arp_sip) = dev_ip_addr_;
            }
        }

        return tx_reply;
    }

    /**
     * @brief Handles an ICMP request packet.
     *
     * Checks if the packet is an ICMP echo request and then builds a reply and sets the appropriate
     * fields.
     *
     * @param [in] pkt_ether_hdr A pointer to the Ethernet header of the packet.
     * @param [in] pkt_ipv4_hdr A pointer to the IPv4 header of the packet.
     * @param [in] pkt_icmp_hdr A pointer to the ICMP header of the packet.
     * @return true if the packet is handled and a reply is sent, false otherwise.
     */
    bool PacketRxCore::handle_icmp_request(
        struct rte_ether_hdr **pkt_ether_hdr, struct rte_ipv4_hdr **pkt_ipv4_hdr,
        struct rte_icmp_hdr **pkt_icmp_hdr
    )
    {
        bool tx_reply = false;

        if (((*pkt_icmp_hdr)->icmp_type == RTE_IP_ICMP_ECHO_REQUEST) &&
            ((*pkt_icmp_hdr)->icmp_code == 0))
        {

            LOG4CXX_DEBUG_LEVEL(3, logger_, "RX ICMP ECHO REQUEST: " << lcore_id_
                << " src: " << mac_addr_str((*pkt_ether_hdr)->src_addr)
                << " dst: " << mac_addr_str((*pkt_ether_hdr)->dst_addr)
            );

            tx_reply = true;

            struct rte_ether_addr tmp_ether_addr;
            rte_ether_addr_copy(&((*pkt_ether_hdr)->src_addr), &tmp_ether_addr);
            rte_ether_addr_copy(&((*pkt_ether_hdr)->dst_addr), &((*pkt_ether_hdr)->src_addr));
            rte_ether_addr_copy(&(tmp_ether_addr), &((*pkt_ether_hdr)->dst_addr));

            uint32_t tmp_ip_addr = (*pkt_ipv4_hdr)->src_addr;
            (*pkt_ipv4_hdr)->src_addr = (*pkt_ipv4_hdr)->dst_addr;
            (*pkt_ipv4_hdr)->dst_addr = tmp_ip_addr;

            (*pkt_icmp_hdr)->icmp_type = RTE_IP_ICMP_ECHO_REPLY;

            uint32_t cksum = ~(*pkt_icmp_hdr)->icmp_cksum & 0xFFFF;
            cksum += ~htons(RTE_IP_ICMP_ECHO_REQUEST << 8) & 0xFFFF;
            cksum += htons(RTE_IP_ICMP_ECHO_REPLY << 8);
            cksum = (cksum & 0xffff) + (cksum >> 16);
            cksum = (cksum & 0xffff) + (cksum >> 16);
            (*pkt_icmp_hdr)->icmp_cksum = ~cksum;
        }

        return tx_reply;
    }
    /**
    * @brief Handles an incoming UDP packet.
    *
    * Logs the incoming packet and if the destination port is in the list of allowed RX ports, it
    * will enqueue the packet on the appropriate forwarding ring.
    *
    * @param pkt A pointer to the incoming packet.
    * @param pkt_ether_hdr A pointer to the incoming Ethernet header.
    * @param pkt_ipv4_hdr A pointer to the incoming IPv4 header.
    * @param pkt_udp_hdr A pointer to the incoming UDP header.
    *
    * @return true if the packet is forwarded, false otherwise.
    */

    bool PacketRxCore::handle_udp_packet(
        struct rte_mbuf **pkt, struct rte_ether_hdr **pkt_ether_hdr,
        struct rte_ipv4_hdr **pkt_ipv4_hdr, struct rte_udp_hdr **pkt_udp_hdr
    )
    {
        bool pkt_forwarded = false;

        // Get the destination port from the UDP packet
        uint16_t dst_port = rte_bswap16((*pkt_udp_hdr)->dst_port);

        LOG4CXX_DEBUG_LEVEL(3, logger_, "RX UDP: " << lcore_id_
            << " src: " << mac_addr_str((*pkt_ether_hdr)->src_addr)
            << " dst: " << mac_addr_str((*pkt_ether_hdr)->dst_addr)
            << " len: " << rte_bswap16((*pkt_udp_hdr)->dgram_len)
            << " rx port: " << dst_port
        );

        // If the destination port is in the list of allowed RX ports continue to process the
        // packet
        if (std::find(config_.rx_ports_.begin(), config_.rx_ports_.end(), dst_port) !=
            config_.rx_ports_.end())
        {

            // Get the protocol header from the start of the UDP payload and resolve the frame
            // number
            PacketHeader* pkt_header =
                (PacketHeader *)((uint8_t *)*pkt_udp_hdr + sizeof(struct rte_udp_hdr));
            uint64_t frame_number = decoder_->get_frame_number(pkt_header);

            LOG4CXX_DEBUG_LEVEL(3, logger_, "RX UDP: " << lcore_id_
                << " protocol header: frame: " << frame_number
                << " packet: " << decoder_->get_packet_number(pkt_header)
            );

            // Queue the packet on the appropriate forwarding ring based on the frame number
            int rc = rte_ring_enqueue(
                packet_forward_rings_[frame_number % config_.num_downstream_cores], *pkt
            );

            // If the queueing failed, attempt to retry
            if (unlikely(rc != 0))
            {
                uint32_t retry = 0;
                while ((rc != 0) && (retry++ < config_.max_packet_queue_retries_))
                {
                    rte_delay_us(1);
                    rc = rte_ring_enqueue(
                        packet_forward_rings_[frame_number % config_.num_downstream_cores],
                        *pkt
                    );
                }
            }

            if (likely(rc == 0))
            {
                pkt_forwarded = true;
            }
        }

        return pkt_forwarded;
    }

    void PacketRxCore::status(OdinData::IpcMessage& status, const std::string& path)
    {

        LOG4CXX_DEBUG(logger_, "Status requested for packetrxcore_" << port_id_
            << " from the DPDK plugin");

        std::string status_path = path + "/packetrxcore_" + std::to_string(port_id_) + "/";

        status.set_param(status_path + "total_packets", packet_counter_);
    }

    bool PacketRxCore::connect(void)
    {  
        LOG4CXX_INFO(logger_, config_.core_name << " : " << proc_idx_ << " Has no upstream resources.");
        
        return true;
    }

    DPDKREGISTER(DpdkWorkerCore, PacketRxCore, "PacketRxCore");
}
