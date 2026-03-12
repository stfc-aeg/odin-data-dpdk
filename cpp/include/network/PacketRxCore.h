#ifndef INCLUDE_PACKETRXCORE_H_
#define INCLUDE_PACKETRXCORE_H_

#include <atomic>
#include <set>
#include <string>
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

    class PacketRxCore : public DpdkWorkerCore
    {
    public:

        PacketRxCore(
            int proc_idx_, int socket_id, DpdkWorkCoreReferences dpdkWorkCoreReferences
        );
        ~PacketRxCore();

        bool run(unsigned int lcore_id);
        void stop(void);
        void status(OdinData::IpcMessage& status, const std::string& path);
        bool connect(void);
        void configure(OdinData::IpcMessage& config);
        void execute(const std::string& command, OdinData::IpcMessage& reply) override;
        std::vector<std::string> requestCommands() override;

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
        uint16_t port_id_;
        bool device_configured_;
        PacketProtocolDecoder* decoder_;

        int64_t first_frame_number_;
        uint64_t first_seen_frame_number_;
        uint64_t rx_frames_;
        bool rx_enable_;


        struct rte_ether_addr dev_eth_addr_;
        uint32_t dev_ip_addr_;
        std::vector<struct rte_ring *> packet_forward_rings_;
        struct rte_ring *packet_release_ring_;

        LoggerPtr logger_;
    };
}

#endif // INCLUDE_PACKETRXCORE_H_