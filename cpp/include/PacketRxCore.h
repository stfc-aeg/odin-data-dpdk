#ifndef INCLUDE_PACKETRXCORE_H_
#define INCLUDE_PACKETRXCORE_H_

#include <set>
#include <string>
#include <vector>

#include <log4cxx/logger.h>
using namespace log4cxx;
using namespace log4cxx::helpers;
#include <DebugLevelLogger.h>

#include "DpdkWorkerCore.h"
#include "DpdkCoreConfiguration.h"
#include "PacketRxConfiguration.h"
#include "ProtocolDecoder.h"

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


    private:

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

        PacketRxConfiguration config_;

        int proc_idx_;
        uint64_t packet_counter_;
        uint16_t port_id_;
        ProtocolDecoder* decoder_;

        struct rte_ether_addr dev_eth_addr_;
        uint32_t dev_ip_addr_;
        std::vector<struct rte_ring *> packet_forward_rings_;
        struct rte_ring *packet_release_ring_;

        LoggerPtr logger_;
    };
}

#endif // INCLUDE_PACKETRXCORE_H_