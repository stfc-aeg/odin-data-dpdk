#include <algorithm>
#include <sstream>
#include <stdexcept>
#include <cstring>
#include <cstdint>
#include "network/PacketRxCore.h"
#include "DpdkUtils.h"
#include "dpdk_version_compatibiliy.h"

// Offset added to the detected first frame number before latching, giving other
// RxCores time to pick up the shared latch before that frame is due. E.g. if the
// first frame seen is 16 and this is 5, the latch (and all cores' starting frame)
// is set to 21; proc_idx_==0 discards frames 16-20 rather than processing them.
#define FRAME_LATCH_OFFSET 1000

namespace FrameProcessor
{
    // Static definition: proc_idx_==0 sets this latch, all other RxCores adopt it
    std::atomic<int64_t> PacketRxCore::shared_first_frame_number_(-1);

    PacketRxCore::PacketRxCore(
        int proc_idx, int socket_id, DpdkWorkCoreReferences dpdkWorkCoreReferences
    ) :
        DpdkWorkerCore(socket_id),
        proc_idx_(proc_idx),
        decoder_(dynamic_cast<PacketProtocolDecoder *>(dpdkWorkCoreReferences.decoder)),
        logger_(Logger::getLogger("FP.PacketRxCore")),
        dropped_packets_(0),
        captured_packets_(0),
        total_packets_(0),
        arp_replies_(0),
        icmp_replies_(0),
        port_id_(UINT16_MAX),
        device_configured_(false),
        device_(nullptr),
        mean_burst_us_(0),
        max_burst_us_(0),
        max_burst_us_all_time_(0),
        mean_pkts_per_burst_(0),
        max_pkts_per_burst_(0),
        estimated_pps_(0),
        max_estimated_pps_(0),
        latch_pending_(true)
    {

        config_.resolve(dpdkWorkCoreReferences.core_config, dpdkWorkCoreReferences.config_key);

        LOG4CXX_INFO(logger_, "FP.PacketRxCore " << proc_idx_ << " Created with config:"
            << " | core_name: " << config_.core_name
            << " | num_cores: " << config_.num_cores
            << " | connect: " << config_.connect
            << " | num_downstream_cores: " << config_.num_downstream_cores
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
        }

        // DPDK does not implement an IP stack so cannot resolve any kernel-assigned IP address.
        // The device IP must be supplied from config to allow ARP reply generation.
        if (inet_pton(AF_INET, instance_device_ip_.c_str(), &dev_ip_addr_) < 1)
        {
            LOG4CXX_ERROR(logger_, "Error resolving device IP address for port " << port_id_
                << " from value " << instance_device_ip_
            );
        }

        LOG4CXX_DEBUG_LEVEL(2, logger_, "Ethernet device on port " << port_id_
            << " has MAC address " << mac_addr_str(dev_eth_addr_)
            << " IP address " << ip_addr_str(dev_ip_addr_)
        );

        unsigned int ring_size;
        std::string ring_name;

        // Build per-stream branches from the downstream_branches array injected by DpdkCoreManager.
        // Each branch owns its own set of forward rings and per-stream gating state.
        // Multiple PacketRxCore instances share these rings (MP/MC safe), so only the first
        // instance creates them; subsequent instances look them up.
        ring_size = nearest_power_two(config_.fwd_ring_size_);
        {
            const OdinData::ParamContainer::Value* rx_cfg =
                dpdkWorkCoreReferences.core_config.get_worker_core_config(config_.config_key);

            if (rx_cfg && rx_cfg->HasMember("downstream_branches") &&
                (*rx_cfg)["downstream_branches"].IsArray())
            {
                for (auto& branch_val : (*rx_cfg)["downstream_branches"].GetArray())
                {
                    StreamBranch branch;
                    if (branch_val.HasMember("config_key") && branch_val["config_key"].IsString())
                        branch.config_key = branch_val["config_key"].GetString();
                    if (branch_val.HasMember("stream_id") && branch_val["stream_id"].IsString())
                        branch.stream_id = branch_val["stream_id"].GetString();
                    if (branch_val.HasMember("decoder_mode") && branch_val["decoder_mode"].IsString())
                        branch.decoder_mode = branch_val["decoder_mode"].GetString();
                    if (branch_val.HasMember("num_cores") && branch_val["num_cores"].IsInt())
                        branch.num_cores = branch_val["num_cores"].GetUint();
                    if (branch_val.HasMember("rx_ports") && branch_val["rx_ports"].IsArray())
                    {
                        for (auto& p : branch_val["rx_ports"].GetArray())
                            branch.rx_ports.push_back(static_cast<uint16_t>(p.GetInt()));
                    }
                    branch.frame_outer_chunk_size =
                        decoder_->get_frame_outer_chunk_size(branch.decoder_mode);

                    for (unsigned int core_idx = 0; core_idx < branch.num_cores; core_idx++)
                    {
                        ring_name = ring_name_pkt_fwd(branch.config_key, socket_id_, core_idx);
                        struct rte_ring* fwd_ring = rte_ring_lookup(ring_name.c_str());
                        if (fwd_ring == NULL)
                        {
                            LOG4CXX_INFO(logger_, "Creating packet forward ring " << ring_name
                                << " size " << ring_size << " numa " << socket_id_);
                            fwd_ring = rte_ring_create(ring_name.c_str(), ring_size, socket_id_, 0);
                            if (fwd_ring == NULL)
                                throw std::runtime_error("Failed to create forward ring "
                                    + ring_name + ": " + rte_strerror(rte_errno));
                        }
                        else
                        {
                            LOG4CXX_DEBUG_LEVEL(2, logger_, "Forward ring " << ring_name << " reusing");
                        }
                        branch.fwd_rings.push_back(fwd_ring);
                    }

                    LOG4CXX_INFO(logger_, "Branch '" << branch.config_key
                        << "' stream='" << branch.stream_id
                        << "' num_cores=" << branch.num_cores
                        << " rx_ports=[" << [&](){
                            std::ostringstream ps;
                            for (size_t i = 0; i < branch.rx_ports.size(); i++) {
                                if (i) ps << ","; ps << branch.rx_ports[i];
                            } return ps.str(); }() << "]");

                    size_t branch_idx = branches_.size();
                    for (uint16_t port : branch.rx_ports)
                        port_to_branch_.emplace_back(port, branch_idx);

                    branches_.push_back(std::move(branch));
                }
            }

            // Fallback for single-stream configs with no downstream_branches: build one branch
            // from the flat num_downstream_cores and packet_rx's own rx_ports list.
            if (branches_.empty())
            {
                LOG4CXX_INFO(logger_, "No downstream_branches found; building single branch from "
                    "num_downstream_cores=" << config_.num_downstream_cores);
                StreamBranch branch;
                branch.config_key = config_.config_key;
                branch.stream_id  = "";
                branch.num_cores  = config_.num_downstream_cores;
                branch.rx_ports   = config_.rx_ports_;
                branch.frame_outer_chunk_size =
                    decoder_->get_frame_outer_chunk_size(branch.decoder_mode);

                for (unsigned int core_idx = 0; core_idx < branch.num_cores; core_idx++)
                {
                    ring_name = ring_name_pkt_fwd(branch.config_key, socket_id_, core_idx);
                    struct rte_ring* fwd_ring = rte_ring_lookup(ring_name.c_str());
                    if (fwd_ring == NULL)
                    {
                        fwd_ring = rte_ring_create(ring_name.c_str(), ring_size, socket_id_, 0);
                        if (fwd_ring == NULL)
                            throw std::runtime_error("Failed to create forward ring "
                                + ring_name + ": " + rte_strerror(rte_errno));
                    }
                    branch.fwd_rings.push_back(fwd_ring);
                }

                size_t branch_idx = branches_.size();
                for (uint16_t port : branch.rx_ports)
                    port_to_branch_.emplace_back(port, branch_idx);

                branches_.push_back(std::move(branch));
            }
        }

        LOG4CXX_INFO(logger_, "PacketRxCore " << proc_idx_ << " built " << branches_.size()
            << " stream branch(es), " << port_to_branch_.size() << " port mapping(s):");
        for (const auto& b : branches_)
        {
            std::ostringstream ps;
            for (size_t i = 0; i < b.rx_ports.size(); i++) { if (i) ps << ","; ps << b.rx_ports[i]; }
            LOG4CXX_INFO(logger_, "  branch '" << b.config_key << "' stream='" << b.stream_id
                << "' mode='" << b.decoder_mode
                << "' num_cores=" << b.num_cores
                << " rings=" << b.fwd_rings.size()
                << " ports=[" << ps.str() << "]");
        }

        // Create or look up the packet release ring. Multiple PacketRxCore instances share this
        // ring; only the first instance creates it, subsequent instances look it up.
        // Packet release ring is shared across all streams; always unscoped.
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
                throw std::runtime_error("Failed to create packet release ring " + ring_name
                    + ": " + rte_strerror(rte_errno));
            }
        }
        else
        {
            LOG4CXX_DEBUG_LEVEL(2, logger_, "Packet release ring " << ring_name
                << " already exists, reusing"
            );
        }

        if (config_.rx_ports_.size() == 0)
        {
            LOG4CXX_ERROR(logger_, "No RX ports defined");
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

        // Clear branch ring references. Rings are shared across multiple PacketRxCore instances
        // so we do not free them here — DPDK EAL teardown handles cleanup on process exit.
        for (auto& branch : branches_)
            branch.fwd_rings.clear();
        branches_.clear();
        port_to_branch_.clear();
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

        if (!device_configured_ || !device_) {
            LOG4CXX_ERROR(logger_, "No device configured. Stopping RxCore.");
            return false;
        }

        LOG4CXX_INFO(logger_, "PacketRxCore " << lcore_id_
            << " entering rx loop: port_id=" << port_id_
            << " rx_queue_id=" << config_.rx_queue_id_
            << " rx_burst_size=" << config_.rx_burst_size_
            << " branches=" << branches_.size());

        // Drain any packets that arrived between rte_eth_dev_start() (called on the main
        // lcore during construction) and now. These pre-queued packets are freed rather
        // than forwarded since the processor cores aren't ready yet.
        if (proc_idx_ == 0)
        {
            uint32_t drained = 0;
            struct rte_mbuf *drain_bufs[128];
            uint16_t n;
            while ((n = rte_eth_rx_burst(port_id_, config_.rx_queue_id_, drain_bufs, 128)) > 0)
            {
                rte_pktmbuf_free_bulk(drain_bufs, n);
                drained += n;
            }
            if (drained > 0)
                LOG4CXX_INFO(logger_, "PacketRxCore " << lcore_id_
                    << " drained " << drained << " pre-queued packets before polling loop");
        }

        // Confirm mbuf pool is healthy before entering the loop
        {
            std::string pool_name = mbuf_pool_name_str(socket_id_);
            struct rte_mempool* pool = rte_mempool_lookup(pool_name.c_str());
            if (pool)
                LOG4CXX_INFO(logger_, "mbuf pool '" << pool_name
                    << "' avail=" << rte_mempool_avail_count(pool)
                    << " in_use=" << rte_mempool_in_use_count(pool));
            else
                LOG4CXX_ERROR(logger_, "mbuf pool '" << pool_name << "' NOT FOUND");
        }

        // Per-burst processing timing (see mean_burst_us_ etc. in the header for why: a
        // Python-side status poll every 500ms-5s can miss a brief burst that maxed out
        // processing time entirely, since it only ever samples whatever counters look like
        // at poll time). Aggregated every second, same convention as FrameWrapperCore::run().
        uint64_t timing_last = rte_get_tsc_cycles();
        uint64_t cycles_per_sec = rte_get_tsc_hz();
        uint64_t bursts_this_second = 0;
        uint64_t pkts_this_second = 0;
        uint64_t cycles_this_second = 0;
        uint64_t max_burst_cycles_this_second = 0;
        uint16_t max_pkts_this_second = 0;

        while (likely(run_lcore_))
        {
            // Follower cores adopt the shared latch as soon as the leader (proc_idx_==0) sets it,
            // without waiting for the next packet to arrive. Apply to all branches that haven't
            // yet latched. Skipped once every branch has latched (latch_pending_ false), so this
            // doesn't walk branches_ on every poll for the whole steady-state of a capture.
            if (unlikely(proc_idx_ != 0 && latch_pending_.load(std::memory_order_acquire)))
            {
                int64_t shared_latch = shared_first_frame_number_.load(std::memory_order_acquire);
                if (shared_latch != -1)
                {
                    for (auto& branch : branches_)
                    {
                        if (branch.first_frame_number == -1)
                            branch.first_frame_number = shared_latch;
                    }
                    // All previously-unlatched branches were just latched above, so there is
                    // nothing left pending until the next start_capture()/configure() reset.
                    latch_pending_.store(false, std::memory_order_release);
                }
            }

            uint16_t num_rx_pkts = rte_eth_rx_burst(
                port_id_, config_.rx_queue_id_, pkt_bufs, config_.rx_burst_size_
            );

            uint64_t burst_start_cycles = num_rx_pkts > 0 ? rte_get_tsc_cycles() : 0;

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
                        if (pkt_tx_reply) arp_replies_++;
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
                                if (pkt_tx_reply) icmp_replies_++;
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

            if (num_rx_pkts > 0)
            {
                uint64_t burst_cycles = rte_get_tsc_cycles() - burst_start_cycles;
                bursts_this_second++;
                pkts_this_second += num_rx_pkts;
                cycles_this_second += burst_cycles;
                if (burst_cycles > max_burst_cycles_this_second)
                    max_burst_cycles_this_second = burst_cycles;
                if (num_rx_pkts > max_pkts_this_second)
                    max_pkts_this_second = num_rx_pkts;
            }

            uint64_t timing_now = rte_get_tsc_cycles();
            if (unlikely((timing_now - timing_last) >= cycles_per_sec))
            {
                if (bursts_this_second > 0)
                {
                    mean_burst_us_ = (cycles_this_second * 1000000) / (bursts_this_second * cycles_per_sec);
                    max_burst_us_ = (max_burst_cycles_this_second * 1000000) / cycles_per_sec;
                    mean_pkts_per_burst_ = pkts_this_second / bursts_this_second;
                    max_pkts_per_burst_ = max_pkts_this_second;
                    // Sustained throughput capacity this second, derived from mean burst
                    // processing time rather than the fastest burst, so this reflects
                    // realistic steady-state performance rather than a best-case ceiling.
                    estimated_pps_ = mean_burst_us_ > 0
                        ? (mean_pkts_per_burst_ * 1000000) / mean_burst_us_
                        : 0;

                    if (max_burst_us_ > max_burst_us_all_time_)
                        max_burst_us_all_time_ = max_burst_us_;
                    if (estimated_pps_ > max_estimated_pps_)
                        max_estimated_pps_ = estimated_pps_;
                }

                bursts_this_second = 0;
                pkts_this_second = 0;
                cycles_this_second = 0;
                max_burst_cycles_this_second = 0;
                max_pkts_this_second = 0;
                timing_last = timing_now;
            }

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

        uint16_t dst_port = rte_bswap16((*pkt_udp_hdr)->dst_port);

        // Route the packet to the branch that owns this destination port. Linear scan over a
        // small vector: real configs have only a handful of branches/ports per RX core, so this
        // beats unordered_map's hashing/bucket overhead here.
        size_t branch_idx = SIZE_MAX;
        for (const auto& mapping : port_to_branch_)
        {
            if (mapping.first == dst_port)
            {
                branch_idx = mapping.second;
                break;
            }
        }
        if (unlikely(branch_idx == SIZE_MAX))
        {
            return pkt_forwarded;  // port not mapped to any stream
        }

        StreamBranch& branch = branches_[branch_idx];

        if (unlikely(!branch.rx_enable))
        {
            return pkt_forwarded;
        }

        PacketHeader* pkt_header =
            (PacketHeader *)((uint8_t *)*pkt_udp_hdr + sizeof(struct rte_udp_hdr));

        uint64_t packet_number = decoder_->get_packet_number(pkt_header);
        uint64_t frame_number  = decoder_->get_frame_number(pkt_header);

        // Per-stream frame latch: proc_idx_==0 leads; followers adopt via the shared atomic.
        // The shared latch is stream-agnostic because PacketRxCore is shared across streams
        // and the latch only synchronises instances of the same core, not streams.
        if (unlikely(branch.first_frame_number == -1))
        {
            if (proc_idx_ == 0)
            {
                if (packet_number == 0 || frame_number > branch.first_seen_frame_number)
                {
                    branch.first_frame_number =
                        static_cast<int64_t>(frame_number) + FRAME_LATCH_OFFSET;
                    shared_first_frame_number_.store(
                        branch.first_frame_number, std::memory_order_release);
                    // LOG4CXX_INFO(logger_, config_.core_name << " : " << proc_idx_
                    //     << " [" << branch.stream_id << "] Frame latch set to: "
                    //     << branch.first_frame_number);
                }
                else
                {
                    branch.first_seen_frame_number = frame_number;
                    return pkt_forwarded;
                }

                // The offset pushes the latch beyond the frame that triggered it, so
                // that frame (and any before the latch) must be discarded here rather
                // than falling through to the unsigned subtraction below, which would
                // otherwise underflow.
                if (frame_number < static_cast<uint64_t>(branch.first_frame_number))
                {
                    return pkt_forwarded;
                }
            }
            else
            {
                int64_t shared_latch = shared_first_frame_number_.load(std::memory_order_acquire);
                if (shared_latch == -1)
                {
                    return pkt_forwarded;
                }
                branch.first_frame_number = shared_latch;
                // LOG4CXX_INFO(logger_, config_.core_name << " : " << proc_idx_
                //     << " [" << branch.stream_id << "] Adopted frame latch: "
                //     << branch.first_frame_number);
            }
        }

        uint64_t current_frame_number =
            frame_number - static_cast<uint64_t>(branch.first_frame_number);

        // Discard packets beyond this stream's acquisition window
        if (branch.rx_frames != 0 && current_frame_number >= branch.rx_frames)
        {
            return pkt_forwarded;
        }

        if (unlikely(branch.num_cores == 0 || branch.fwd_rings.empty()))
        {
            LOG4CXX_WARN(logger_, "PacketRxCore [" << branch.stream_id
                << "] has no forward rings, dropping packet");
            return pkt_forwarded;
        }

        size_t ring_idx =
            (current_frame_number / branch.frame_outer_chunk_size) % branch.num_cores;

        int rc = rte_ring_enqueue(branch.fwd_rings[ring_idx], *pkt);

        if (unlikely(rc != 0))
        {
            // Don't retry: the downstream core is backed up regardless of how many times we
            // immediately re-try the same full ring, and spinning here only steals cycles from
            // packet reception. Log the drop, but rate-limited - under sustained backpressure
            // this can otherwise fire on every packet, turning a downstream slowdown into an
            // RX-core stall from logging alone.
            if ((branch.ring_full_drops++ % 10000) == 0)
            {
                LOG4CXX_WARN(logger_, "PacketRxCore [" << branch.stream_id
                    << "] ring full, dropped " << branch.ring_full_drops << " packet(s) so far");
            }
        }
        else
        {
            pkt_forwarded = true;
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

        status.set_param(status_path + "total_packets", total_packets_);
        status.set_param(status_path + "dropped_packets", dropped_packets_);
        status.set_param(status_path + "captured_packets", captured_packets_);
        status.set_param(status_path + "arp_replies", arp_replies_);
        status.set_param(status_path + "icmp_replies", icmp_replies_);

        // Per-burst processing timing - see mean_burst_us_ etc. in the header for why these
        // are tracked natively rather than relying on the Python side sampling fast enough.
        status.set_param(status_path + "mean_burst_us", mean_burst_us_);
        status.set_param(status_path + "max_burst_us", max_burst_us_);
        status.set_param(status_path + "max_burst_us_all_time", max_burst_us_all_time_);
        status.set_param(status_path + "mean_pkts_per_burst", mean_pkts_per_burst_);
        status.set_param(status_path + "max_pkts_per_burst", max_pkts_per_burst_);
        status.set_param(status_path + "estimated_pps", estimated_pps_);
        status.set_param(status_path + "max_estimated_pps", max_estimated_pps_);

        // Per-stream branch state
        for (const auto& branch : branches_)
        {
            const std::string bpath = status_path + "stream_" + branch.stream_id + "/";
            status.set_param(bpath + "config_key",          branch.config_key);
            status.set_param(bpath + "rx_enable",           branch.rx_enable);
            status.set_param(bpath + "rx_frames",           branch.rx_frames);
            status.set_param(bpath + "first_frame_number",  branch.first_frame_number);
            status.set_param(bpath + "first_seen_frame_number", branch.first_seen_frame_number);
            status.set_param(bpath + "num_cores",           (uint64_t)branch.num_cores);
            status.set_param(bpath + "ring_full_drops",     branch.ring_full_drops);
        }

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

        // Lookup mbuf pool by its well-known name to report occupancy without requiring a DpdkDevice getter
        if (device_) {
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

        // Per-branch forward ring monitoring
        for (const auto& branch : branches_)
        {
            const std::string bpath = status_path + "stream_" + branch.stream_id + "/";
            for (size_t i = 0; i < branch.fwd_rings.size(); ++i)
            {
                if (branch.fwd_rings[i])
                {
                    std::string rpath = bpath + "forward_ring_" + std::to_string(i) + "_";
                    uint64_t cnt  = (uint64_t)rte_ring_count(branch.fwd_rings[i]);
                    uint64_t free = (uint64_t)rte_ring_free_count(branch.fwd_rings[i]);
                    uint64_t sz   = (uint64_t)rte_ring_get_size(branch.fwd_rings[i]);
                    status.set_param(rpath + "count", cnt);
                    status.set_param(rpath + "free",  free);
                    status.set_param(rpath + "size",  sz);
                    status.set_param(rpath + "utilization_pct",
                        sz > 0 ? (cnt * 100) / sz : (uint64_t)0);
                }
            }
        }

        // Additional performance metrics
        status.set_param(status_path + "num_branches", (uint64_t)branches_.size());
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
        LOG4CXX_INFO(logger_, config_.core_name << " : " << proc_idx_ << " Got update config.");

        // Config applies to every branch; each stream tracks its own frame counters and latch
        // state independently, so there is no need to address them separately.
        for (auto& branch : branches_)
        {
            if (config.has_param("rx_enable"))
            {
                branch.rx_enable = config.get_param("rx_enable", false);
                LOG4CXX_INFO(logger_, config_.core_name << " : " << proc_idx_
                    << " [" << branch.stream_id << "] rx_enable=" << branch.rx_enable);
            }

            // Reset the frame latch whenever capture is going inactive
            if (!branch.rx_enable)
            {
                branch.first_frame_number      = -1;
                branch.first_seen_frame_number = 0;
                latch_pending_.store(true, std::memory_order_release);
                if (proc_idx_ == 0)
                    shared_first_frame_number_.store(-1, std::memory_order_release);
                branch.rx_frames = config.get_param("rx_frames", branch.rx_frames);
                LOG4CXX_INFO(logger_, config_.core_name << " : " << proc_idx_
                    << " [" << branch.stream_id << "] Reset latch, rx_frames=" << branch.rx_frames);
            }
        }
    }

    std::vector<std::pair<std::string, int>> PacketRxCore::requestCommands()
    {
        return {
            {"start_capture", DEFAULT_COMMAND_PRIORITY},
            {"stop_capture",  DEFAULT_COMMAND_PRIORITY}
        };
    }

    void PacketRxCore::execute(const std::string& command, OdinData::IpcMessage& reply)
    {
        if (command == "start_capture")
        {
            start_capture(reply);
        }
        else if (command == "stop_capture")
        {
            stop_capture(reply);
        }
        else
        {
            reply.set_nack("PacketRxCore: unknown command: " + command);
        }
    }

    void PacketRxCore::start_capture(OdinData::IpcMessage& reply)
    {
        LOG4CXX_INFO(logger_, config_.core_name << " : " << proc_idx_
                << " Called start_capture");

        bool already_running = false;
        for (auto& branch : branches_)
        {
            if (branch.rx_enable) { already_running = true; break; }
        }
        if (already_running)
        {
            reply.set_nack("PacketRxCore: capture already running");
            return;
        }

        if (proc_idx_ == 0)
            shared_first_frame_number_.store(-1, std::memory_order_release);

        for (auto& branch : branches_)
        {
            branch.first_frame_number      = -1;
            branch.first_seen_frame_number = 0;
            branch.rx_enable               = true;
            latch_pending_.store(true, std::memory_order_release);
            LOG4CXX_INFO(logger_, config_.core_name << " : " << proc_idx_
                << " [" << branch.stream_id << "] start_capture");
        }
    }

    void PacketRxCore::stop_capture(OdinData::IpcMessage& reply)
    {
        bool already_stopped = true;
        for (auto& branch : branches_)
        {
            if (branch.rx_enable) { already_stopped = false; break; }
        }
        if (already_stopped)
        {
            reply.set_nack("PacketRxCore: capture already stopped");
            return;
        }

        if (proc_idx_ == 0)
            shared_first_frame_number_.store(-1, std::memory_order_release);

        for (auto& branch : branches_)
        {
            branch.rx_enable               = false;
            branch.first_frame_number      = -1;
            branch.first_seen_frame_number = 0;
            latch_pending_.store(true, std::memory_order_release);
            LOG4CXX_INFO(logger_, config_.core_name << " : " << proc_idx_
                << " [" << branch.stream_id << "] stop_capture");
        }
    }

    DPDKREGISTER(DpdkWorkerCore, PacketRxCore, "PacketRxCore");
}
