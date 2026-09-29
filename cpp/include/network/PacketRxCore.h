#ifndef INCLUDE_PACKETRXCORE_H_
#define INCLUDE_PACKETRXCORE_H_

#include <atomic>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <log4cxx/logger.h>
using namespace log4cxx;
using namespace log4cxx::helpers;
#include <DebugLevelLogger.h>

#include "DpdkWorkerCore.h"
#include "DpdkCoreConfiguration.h"
#include "network/PacketRxConfiguration.h"
#include "network/PacketProtocolDecoder.h"
#include "DpdkDevice.h"

#include <rte_ether.h>
#include <rte_ring.h>
#include <rte_mbuf.h>
#include <rte_ethdev.h>

namespace FrameProcessor
{
    // Per-stream branch: owns one set of forward rings and all gating state for that stream.
    struct StreamBranch
    {
        std::string config_key;                    // downstream config key (e.g. "packet_processor_aux")
        std::string stream_id;                     // stream name (e.g. "auxiliary")
        std::string decoder_mode;                  // mode string for decoder calls (e.g. "frame_512x512")
        std::vector<uint16_t> rx_ports;            // UDP destination ports that map to this stream
        unsigned int num_cores;                    // number of downstream processor cores

        std::vector<struct rte_ring*> fwd_rings;   // one ring per downstream core

        // Per-stream gating state (mirrors the global state in single-stream builds)
        bool rx_enable;
        uint64_t rx_frames;                        // 0 = unlimited
        int64_t first_frame_number;
        uint64_t first_seen_frame_number;

        // Cached from decoder_->get_frame_outer_chunk_size(decoder_mode) once at branch build
        // time, since decoder_mode is fixed per branch - avoids a virtual call plus a
        // string-keyed mode lookup on every packet in the hot path.
        uint64_t frame_outer_chunk_size;

        // Count of packets dropped because their forward ring was full. Used to rate-limit the
        // "ring full" warning so sustained downstream backpressure doesn't turn into a log call
        // (and stream write) on every single packet, which only makes the RX core fall further
        // behind.
        uint64_t ring_full_drops;

        StreamBranch() :
            num_cores(0), rx_enable(false), rx_frames(0),
            first_frame_number(-1), first_seen_frame_number(0), ring_full_drops(0),
            frame_outer_chunk_size(1)
        {}
    };

    class PacketRxCore : public DpdkWorkerCore
    {
    public:

        PacketRxCore(
            int proc_idx, int socket_id, DpdkWorkCoreReferences dpdkWorkCoreReferences
        );
        ~PacketRxCore();

        bool run(unsigned int lcore_id);
        void stop(void);
        void status(OdinData::IpcMessage& status, const std::string& path);
        bool connect(void);
        void configure(OdinData::IpcMessage& config);
        void execute(const std::string& command, OdinData::IpcMessage& reply) override;
        void requestConfiguration(OdinData::IpcMessage& reply);
        std::vector<std::pair<std::string, int>> requestCommands() override;

    private:
        void start_capture(OdinData::IpcMessage& reply);
        void stop_capture(OdinData::IpcMessage& reply);
        bool add_device(const std::string& pci_address);
        bool remove_device();

        bool handle_arp_request(
            struct rte_ether_hdr **pkt_ether_hdr, struct rte_arp_hdr **pkt_arp_hdr
        );
        bool handle_icmp_request(
            struct rte_ether_hdr **pkt_ether_hdr, struct rte_ipv4_hdr **pkt_ipv4_hdr,
            struct rte_icmp_hdr **pkt_icmp_hdr
        );
        bool handle_udp_packet(
            struct rte_mbuf **pkt, struct rte_ether_hdr **pkt_ether_hdr,
            struct rte_ipv4_hdr **pkt_ipv4_hdr, struct rte_udp_hdr **pkt_udp_hdr
        );

        static const uint16_t DEFAULT_BURST_SIZE;
        static const unsigned int DEFAULT_FWD_RING_SIZE;
        static const unsigned int DEFAULT_RELEASE_RING_SIZE;
        static std::atomic<int64_t> shared_first_frame_number_;  //!< Shared latch: proc_idx_==0 sets, others adopt

        PacketRxConfiguration config_;

        DpdkDevice* device_;

        int proc_idx_;
        std::string instance_pcie_device_;  //!< PCIe address for this instance's NIC
        std::string instance_device_ip_;    //!< IP address for this instance's NIC
        uint64_t total_packets_;
        uint64_t dropped_packets_;
        uint64_t captured_packets_;
        uint64_t arp_replies_;
        uint64_t icmp_replies_;
        uint16_t port_id_;
        bool device_configured_;

        // Per-burst processing timing, aggregated over rolling 1-second windows in run() -
        // captures true high-water marks (e.g. a burst that briefly maxed out processing time)
        // that a Python-side status poll every 500ms-5s could otherwise miss entirely between
        // polls. Mirrors the timing convention already used by FrameWrapperCore.
        uint64_t mean_burst_us_;         //!< Mean time to process one rx_burst, over the last second
        uint64_t max_burst_us_;          //!< Max time to process one rx_burst, over the last second
        uint64_t max_burst_us_all_time_; //!< All-time high-water mark for a single burst's processing time
        uint64_t mean_pkts_per_burst_;   //!< Mean packets per burst, over the last second
        uint64_t max_pkts_per_burst_;    //!< Max packets in a single burst, over the last second
        uint64_t estimated_pps_;         //!< mean_pkts_per_burst_ / mean_burst_us_ - sustained throughput capacity
        uint64_t max_estimated_pps_;     //!< All-time high-water mark for estimated_pps_
        PacketProtocolDecoder* decoder_;

        // Per-stream branch state (gating, port routing, forward rings)
        std::vector<StreamBranch> branches_;
        // Port -> branch index lookup (populated in constructor). Linear-scanned rather than
        // hashed: real configs have only a handful of branches/ports per RX core, so a small
        // vector scan beats unordered_map's hashing/bucket overhead on the per-packet path.
        std::vector<std::pair<uint16_t, size_t>> port_to_branch_;

        // True while any branch still needs to adopt the shared frame latch. Sub-leader cores
        // (proc_idx_ != 0) check this once per poll iteration to skip the per-branch adoption
        // scan entirely once every branch has latched, instead of walking branches_ every burst.
        // Atomic because it's set from start_capture()/stop_capture()/configure() on the control
        // thread and read/cleared from run() on the worker lcore thread - a plain bool here would
        // be a data race with no guarantee the worker thread ever observes the new value.
        std::atomic<bool> latch_pending_;

        struct rte_ether_addr dev_eth_addr_;
        uint32_t dev_ip_addr_;
        struct rte_ring *packet_release_ring_;

        LoggerPtr logger_;
    };
}

#endif // INCLUDE_PACKETRXCORE_H_