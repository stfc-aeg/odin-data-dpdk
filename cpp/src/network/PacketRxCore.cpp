#include <algorithm>
#include <sstream>
#include <cstring>
#include "network/PacketRxCore.h"
#include "DpdkUtils.h"
#include "dpdk_version_compatibiliy.h"

namespace FrameProcessor
{
    PacketRxCore::PacketRxCore(
        int proc_idx, int socket_id, DpdkWorkCoreReferences dpdkWorkCoreReferences
    ) :
        DpdkWorkerCore(socket_id),
        proc_idx_(proc_idx),
        decoder_(dynamic_cast<PacketProtocolDecoder *>(dpdkWorkCoreReferences.decoder)),
        logger_(Logger::getLogger("FP.PacketRxCore")),
        first_frame_number_(-1),
        rx_enable_(false),
        rx_frames_(0),
        first_seen_frame_number_(-1),
        dropped_packets_(0),
        captured_packets_(0),
        total_packets_(0),
        port_id_(UINT16_MAX),
        device_configured_(false),
        device_(nullptr)
    {

        // Resolve configuration parameters for athis core from the config object passed as an
        // argument, and the current port ID
        config_.resolve(dpdkWorkCoreReferences.core_config);

        LOG4CXX_INFO(logger_, "FP.PacketRxCore " << proc_idx_ << " Created with config:"
            << " | core_name" << config_.core_name
            << " | num_cores: " << config_.num_cores
            << " | connect: " << config_.connect
            << " | num_downsteam_cores: " << config_.num_downstream_cores
        );


        // Select per-instance PCIe device and IP from the config vectors using proc_idx_.
        // num_cores must match the length of pcie_device and device_ip arrays in config.
        if (proc_idx_ < (int)config_.pcie_device_.size()) {
            instance_pcie_device_ = config_.pcie_device_[proc_idx_];
        } else {
            LOG4CXX_ERROR(logger_, "PacketRxCore " << proc_idx_ << ": pcie_device array has "
                << config_.pcie_device_.size() << " entries but num_cores is " << config_.num_cores
                << ". Add a pcie_device entry for each core. This core will not receive packets.");
        }
        if (proc_idx_ < (int)config_.device_ip_.size()) {
            instance_device_ip_ = config_.device_ip_[proc_idx_];
        } else {
            LOG4CXX_ERROR(logger_, "PacketRxCore " << proc_idx_ << ": device_ip array has "
                << config_.device_ip_.size() << " entries but num_cores is " << config_.num_cores
                << ". Add a device_ip entry for each core. This core will not receive packets.");
        }

        // Add devices provided in the configuration
        if (!instance_pcie_device_.empty()) {
            if (!add_device(instance_pcie_device_)) {
                LOG4CXX_ERROR(logger_, "Failed to add device specified in initial configuration: " << instance_pcie_device_);
            }
        }

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
        if (inet_pton(AF_INET, instance_device_ip_.c_str(), &dev_ip_addr_) < 1)
        {
            LOG4CXX_ERROR(logger_, "Error resolving device IP address for port " << port_id_
                << " from value" << instance_device_ip_
            );
            // TODO - raise exception here?
        }

        LOG4CXX_DEBUG_LEVEL(2, logger_, "Ethernet device on port " << port_id_
            << " has MAC address " << mac_addr_str(dev_eth_addr_)
            << " IP address " << ip_addr_str(dev_ip_addr_)
        );

        unsigned int ring_size;
        std::string ring_name;

        // Create or look up packet forwarding rings for each of the packet processing cores.
        // Multiple PacketRxCore instances share these rings (MP/MC safe - no SP/SC flags),
        // so only the first instance creates them; subsequent instances look them up.
        ring_size = nearest_power_two(config_.fwd_ring_size_);
        for (int core_idx = 0; core_idx < config_.num_downstream_cores; core_idx++)
        {
            ring_name = ring_name_str(config_.core_name, socket_id_, core_idx);
            struct rte_ring *fwd_ring = rte_ring_lookup(ring_name.c_str());
            if (fwd_ring == NULL)
            {
                LOG4CXX_INFO(logger_, "Creating packet forward ring name "
                    << ring_name << " of size " << ring_size << " numa node: " << socket_id_
                );
                fwd_ring = rte_ring_create(ring_name.c_str(), ring_size, socket_id_, 0);
                if (fwd_ring == NULL)
                {
                    LOG4CXX_ERROR(logger_, "Error creating packet forward ring " << ring_name
                        << " : " << rte_strerror(rte_errno)
                    );
                    // TODO - raise exception here?
                }
            }
            else
            {
                LOG4CXX_DEBUG_LEVEL(2, logger_, "Packet forward ring " << ring_name
                    << " already exists, reusing"
                );
            }
            packet_forward_rings_.push_back(fwd_ring);
        }

        // Create or look up the packet release ring. Multiple PacketRxCore instances share this
        // ring; only the first instance creates it, subsequent instances look it up.
        ring_name = ring_name_pkt_release(socket_id_);
        ring_size = nearest_power_two(config_.release_ring_size_);
        packet_release_ring_ = rte_ring_lookup(ring_name.c_str());
        if (packet_release_ring_ == NULL)
        {
            LOG4CXX_DEBUG_LEVEL(2, logger_, "Creating packet release ring name "
                << ring_name << " of size " << ring_size << " numa node: " << socket_id_
            );
            packet_release_ring_ = rte_ring_create(ring_name.c_str(), ring_size, socket_id_, 0);
            if (packet_release_ring_ == NULL)
            {
                LOG4CXX_ERROR(logger_, "Error creating packet release ring " << ring_name
                    << " : " << rte_strerror(rte_errno)
                );
                // TODO - raise exception here?
            }
        }
        else
        {
            LOG4CXX_DEBUG_LEVEL(2, logger_, "Packet release ring " << ring_name
                << " already exists, reusing"
            );
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

        // Clear the local references to shared rings. Rings are shared across multiple
        // PacketRxCore instances, so we do not free them here — DPDK EAL teardown handles
        // ring cleanup on process exit.
        packet_forward_rings_.clear();
        std::vector<struct rte_ring *>(packet_forward_rings_).swap(packet_forward_rings_);
        packet_release_ring_ = nullptr;

        if (device_) {
            remove_device();
        }

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

        uint16_t num_replies = 0;

        bool pkt_tx_reply = false;
        bool pkt_forwarded = false;

        // check to see if a valid device has been configured
        if (!device_configured_ || !device_) {
            LOG4CXX_ERROR(logger_, "No device configured. Stopping RxCore.");
            return false;
        }

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

                                
                                                                
                                break;

                            default:
                                break;

                        } // switch(pkt_ipv4_hdr->next_proto_id)
                        break;

                    default:
                        break;

                } // switch(rte_bswap16(pkt_ether_hdr->ether_type))


                total_packets_++;

                // If a handler wants to send a reply to the packet, add it to the buffer
                // and increment the number of replies. If the packet has been forwarded by a
                // handler (e.g. valid UDP packets) do nothing, otherwise free the packet mbuf
                if (pkt_tx_reply)
                {
                    pkt_bufs[num_replies++] = pkt;
                    dropped_packets_++;
                }
                else if (pkt_forwarded)
                {
                    // Do nothing with the packet - handler has forwarded it
                    captured_packets_++;
                }
                else
                {
                    rte_pktmbuf_free(pkt);
                    dropped_packets_++;
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
                        //rte_delay_us(1);
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
            uint16_t num_released = rte_ring_dequeue_burst(packet_release_ring_, (void **)&release_pkt, config_.rx_burst_size_, NULL);
            if (num_released > 0)
            {
                rte_pktmbuf_free_bulk((struct rte_mbuf **)&release_pkt, num_released);
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

        if (((*pkt_icmp_hdr)->icmp_type == RTE_ICMP_TYPE_ECHO_REQUEST) &&
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

            (*pkt_icmp_hdr)->icmp_type = RTE_ICMP_TYPE_ECHO_REPLY;

            uint32_t cksum = ~(*pkt_icmp_hdr)->icmp_cksum & 0xFFFF;
            cksum += ~htons(RTE_ICMP_TYPE_ECHO_REQUEST << 8) & 0xFFFF;
            cksum += htons(RTE_ICMP_TYPE_ECHO_REPLY << 8);
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

        uint64_t frame_outer_chunk_size = decoder_->get_frame_outer_chunk_size();

        // LOG4CXX_DEBUG_LEVEL(3, logger_, "RX UDP: " << lcore_id_
        //     << " src: " << mac_addr_str((*pkt_ether_hdr)->src_addr)
        //     << " dst: " << mac_addr_str((*pkt_ether_hdr)->dst_addr)
        //     << " len: " << rte_bswap16((*pkt_udp_hdr)->dgram_len)
        //     << " rx port: " << dst_port
        // );

        // If the destination port is in the list of allowed RX ports continue to process the
        // packet
        if (std::find(config_.rx_ports_.begin(), config_.rx_ports_.end(), dst_port) !=
            config_.rx_ports_.end())
        {

            // Get the protocol header from the start of the UDP payload and resolve the frame
            // number
            PacketHeader* pkt_header =
                (PacketHeader *)((uint8_t *)*pkt_udp_hdr + sizeof(struct rte_udp_hdr));


            // check to see if rx_enable is true and if the packet should be discarded or
            // forwarded
            if(unlikely(rx_enable_ == false))
            {
                return pkt_forwarded;
            }
            
            // This statement allows the code to reset the "starting" frame number in code
            // When first_frame_number_ is set to -1 the next first packet of a frame will be use
            // to create a variable to offset the frame number by, allow for frames to be
            // distributed as it the first frame has a frame number of 0

            uint64_t packet_number = decoder_->get_packet_number(pkt_header);
            uint64_t frame_number = decoder_->get_frame_number(pkt_header);

            if(unlikely(first_frame_number_ == -1))
            {

                // Check if this is the first packet of a frame or the first seen packet of a new frame

                if (packet_number == 0 || frame_number > first_seen_frame_number_)
                {
                    first_frame_number_ = frame_number;
                    // LOG4CXX_INFO(logger_, "Frame latch updated to: " << first_frame_number_);
                }
                else
                {
                    first_seen_frame_number_ = frame_number;
                    // If the packet recieved is not the start of the frame, then it is sent
                    // back  to the main fast loop to be discarded
                    return pkt_forwarded;
                }
            }

            uint64_t current_frame_number = frame_number - first_frame_number_;

            // Check to see if the packet recieved is within the current aquisition
            // if not then return this function and discard the packet

            if(rx_frames_ != 0 && current_frame_number >= rx_frames_)
            {
                return pkt_forwarded;
            }


            // LOG4CXX_DEBUG_LEVEL(3, logger_, "RX UDP: " << lcore_id_
            //     << " protocol header: frame: " << current_frame_number
            //     << " packet: " << decoder_->get_packet_number(pkt_header)
            // );

            // Queue the packet on the appropriate forwarding ring based on the frame number
            int rc = rte_ring_enqueue(
                packet_forward_rings_[(current_frame_number / frame_outer_chunk_size) % config_.num_downstream_cores], *pkt
            );

            // If the queueing failed, attempt to retry
            if (unlikely(rc != 0))
            {
                uint32_t retry = 0;
                while ((rc != 0) && (retry++ < config_.max_packet_queue_retries_))
                {
                    //rte_delay_us(1);
                    rc = rte_ring_enqueue(
                        packet_forward_rings_[(current_frame_number / frame_outer_chunk_size) % config_.num_downstream_cores],
                        *pkt
                    );
                }
                LOG4CXX_INFO(logger_, "PacketRxCore failed to enqueue packet, ring full");
            }


            if (likely(rc == 0))
            {
                pkt_forwarded = true;
                // The packet was enqueued to a packet ring, increment the captured packet counter
            }
        }

        return pkt_forwarded;
    }

    bool PacketRxCore::add_device(const std::string& pci_address)
    {
        if (device_configured_) {
            LOG4CXX_WARN(logger_, "Device already configured. Ignoring: " << pci_address);
            return false;
        }

        int ret = rte_eal_hotplug_add("pci", pci_address.c_str(), "");
        if (ret < 0) {
            LOG4CXX_ERROR(logger_, "Failed to hot plug device: " << pci_address);
            return false;
        }

        ret = rte_eth_dev_get_port_by_name(pci_address.c_str(), &port_id_);
        if (ret != 0) {
            LOG4CXX_ERROR(logger_, "Failed to get port ID for device: " << pci_address);
            return false;
        }

        device_ = new DpdkDevice(port_id_, config_.dpdk_device());
        if (!device_->start()) {
            LOG4CXX_ERROR(logger_, "Failed to start device: " << pci_address);
            delete device_;
            device_ = nullptr;
            return false;
        }

        device_configured_ = true;
        LOG4CXX_INFO(logger_, "Successfully added device: " << pci_address << " (Port ID: " << port_id_ << ")");
        return true;
    }
    
    bool PacketRxCore::remove_device()
    {
        if (!device_configured_) {
            return true;
        }

        if (device_) {
            device_->stop();
            delete device_;
            device_ = nullptr;
        }

        int ret = rte_eal_hotplug_remove("pci", instance_pcie_device_.c_str());
        if (ret < 0) {
            LOG4CXX_ERROR(logger_, "Failed to hot unplug device: " << instance_pcie_device_);
            return false;
        }

        device_configured_ = false;
        port_id_ = UINT16_MAX;
        LOG4CXX_INFO(logger_, "Successfully removed device: " << instance_pcie_device_);
        return true;
    }

    void PacketRxCore::status(OdinData::IpcMessage& status, const std::string& path)
    {
        LOG4CXX_DEBUG(logger_, "Status requested for packetrxcore_" << port_id_
            << " from the DPDK plugin");

        std::string status_path = path + "/packetrxcore_" + std::to_string(port_id_) + "/";

        // Original status parameters
        status.set_param(status_path + "total_packets", total_packets_);
        status.set_param(status_path + "dropped_packets", dropped_packets_);
        status.set_param(status_path + "captured_packets", captured_packets_);
        status.set_param(status_path + "rx_enable", rx_enable_);
        status.set_param(status_path + "rx_frames", rx_frames_);
        status.set_param(status_path + "first_seen_frame_number", first_seen_frame_number_);
        status.set_param(status_path + "first_frame_number", first_frame_number_);

        // RX Queue packet count
        if (device_configured_ && port_id_ != UINT16_MAX) {
            int rx_queue_count = rte_eth_rx_queue_count(port_id_, config_.rx_queue_id_);
            if (rx_queue_count >= 0) {
                status.set_param(status_path + "rx_queue_packet_count", (uint64_t)rx_queue_count);
            }
        }

        // Port Extended Statistics (xstats)
        if (device_configured_ && port_id_ != UINT16_MAX) {
            int len = rte_eth_xstats_get(port_id_, NULL, 0);
            if (len > 0) {
                struct rte_eth_xstat *xstats = (struct rte_eth_xstat *)calloc(len, sizeof(*xstats));
                struct rte_eth_xstat_name *xstats_names = (struct rte_eth_xstat_name *)calloc(len, sizeof(*xstats_names));

                if (xstats && xstats_names) {
                    int ret = rte_eth_xstats_get(port_id_, xstats, len);
                    if (ret >= 0 && ret <= len) {
                        ret = rte_eth_xstats_get_names(port_id_, xstats_names, len);
                        if (ret >= 0 && ret <= len) {
                            std::string xstats_path = status_path + "port_xstats/";
                            for (int i = 0; i < len; i++) {
                                if (xstats[i].value > 0) {
                                    std::string stat_name(xstats_names[i].name);
                                    status.set_param(xstats_path + stat_name, xstats[i].value);
                                }
                            }
                        }
                    }
                }

                if (xstats) free(xstats);
                if (xstats_names) free(xstats_names);
            }
        }

        // Memory pool monitoring - requires DpdkDevice::get_mbuf_pool() method
        // TODO: Add getter method to DpdkDevice class: struct rte_mempool* get_mbuf_pool() const { return mbuf_pool_; }
        if (device_) {
            // Try to lookup mbuf pool by name (if mbuf_pool_name_str function is available)
            std::string mbuf_pool_name = mbuf_pool_name_str(socket_id_);
            struct rte_mempool* mbuf_pool = rte_mempool_lookup(mbuf_pool_name.c_str());

            if (mbuf_pool) {
                uint32_t mbuf_avail = rte_mempool_avail_count(mbuf_pool);
                uint32_t mbuf_in_use = rte_mempool_in_use_count(mbuf_pool);
                uint32_t mbuf_total = mbuf_avail + mbuf_in_use;

                status.set_param(status_path + "mbuf_pool_available", mbuf_avail);
                status.set_param(status_path + "mbuf_pool_in_use", mbuf_in_use);
                status.set_param(status_path + "mbuf_pool_total", mbuf_total);
                status.set_param(status_path + "mbuf_pool_utilization_pct",
                                mbuf_total > 0 ? (mbuf_in_use * 100) / mbuf_total : 0);
            }
        }

        // Release ring monitoring
        if (packet_release_ring_) {
            uint64_t release_ring_count = (uint64_t)rte_ring_count(packet_release_ring_);
            uint64_t release_ring_free = (uint64_t)rte_ring_free_count(packet_release_ring_);
            uint64_t release_ring_size = (uint64_t)rte_ring_get_size(packet_release_ring_);

            status.set_param(status_path + "release_ring_count", release_ring_count);
            status.set_param(status_path + "release_ring_free", release_ring_free);
            status.set_param(status_path + "release_ring_size", release_ring_size);
            uint64_t release_utilization_pct = release_ring_size > 0 ? (release_ring_count * 100) / release_ring_size : 0;
            status.set_param(status_path + "release_ring_utilization_pct", release_utilization_pct);
        }

        // Forward rings monitoring
        for (size_t i = 0; i < packet_forward_rings_.size(); ++i) {
            if (packet_forward_rings_[i]) {
                std::string fwd_ring_path = status_path + "forward_ring_" + std::to_string(i) + "_";
                uint64_t fwd_ring_count = (uint64_t)rte_ring_count(packet_forward_rings_[i]);
                uint64_t fwd_ring_free = (uint64_t)rte_ring_free_count(packet_forward_rings_[i]);
                uint64_t fwd_ring_size = (uint64_t)rte_ring_get_size(packet_forward_rings_[i]);

                status.set_param(fwd_ring_path + "count", fwd_ring_count);
                status.set_param(fwd_ring_path + "free", fwd_ring_free);
                status.set_param(fwd_ring_path + "size", fwd_ring_size);
                uint64_t fwd_utilization_pct = fwd_ring_size > 0 ? (fwd_ring_count * 100) / fwd_ring_size : 0;
                status.set_param(fwd_ring_path + "utilization_pct", fwd_utilization_pct);
            }
        }

        // Additional performance metrics
        status.set_param(status_path + "num_downstream_cores", (uint64_t)config_.num_downstream_cores);
        status.set_param(status_path + "rx_burst_size", (uint64_t)config_.rx_burst_size_);
        status.set_param(status_path + "max_packet_queue_retries", (uint64_t)config_.max_packet_queue_retries_);
    }

    bool PacketRxCore::connect(void)
    {  
        LOG4CXX_INFO(logger_, config_.core_name << " : " << proc_idx_ << " Has no upstream resources.");
        
        return true;
    }

    void PacketRxCore::configure(OdinData::IpcMessage& config)
    {
        // Update the config based from the passed IPCmessage

        LOG4CXX_INFO(logger_, config_.core_name << " : " << proc_idx_ << " Got update config.");


        if (config.has_param("rx_enable"))
        {
            
            rx_enable_ = config.get_param("rx_enable", false);
            LOG4CXX_INFO(logger_, config_.core_name << " : " << proc_idx_ << " Setting rx_enable_ to: " <<  rx_enable_);
        }

        // Only update the other config is rx_enable is currently false
        // Whenever rx_enabled is false then make sure first_frame_number_ is ready for when it's turned on
        if (!rx_enable_)
        {   
            first_frame_number_ = -1;
            first_seen_frame_number_ = -1;
            rx_frames_ = config.get_param("rx_frames", rx_frames_);
            LOG4CXX_INFO(logger_, config_.core_name << " : " << proc_idx_ << " Reseting frame latch and setting rx_frames_ to: " <<  rx_frames_);
        }


    }

    DPDKREGISTER(DpdkWorkerCore, PacketRxCore, "PacketRxCore");
}
