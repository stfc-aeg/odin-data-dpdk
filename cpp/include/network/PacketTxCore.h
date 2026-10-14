#ifndef INCLUDE_PacketTxCore_H_
#define INCLUDE_PacketTxCore_H_

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <log4cxx/logger.h>
using namespace log4cxx;
using namespace log4cxx::helpers;
#include <DebugLevelLogger.h>

#include "DpdkWorkerCore.h"
#include "DpdkCoreConfiguration.h"
#include "DpdkDevice.h"
#include "DpdkDeviceConfiguration.h"
#include "PacketTxCoreConfiguration.h"
#include "PacketProtocolDecoder.h"
#include <rte_ring.h>
#include <blosc.h>
#include <rte_byteorder.h>

namespace FrameProcessor {

class PacketTxCore : public DpdkWorkerCore {
public:
    PacketTxCore(int fb_idx, int socket_id, DpdkWorkCoreReferences& dpdkWorkCoreReferences);
    ~PacketTxCore();

    bool run(unsigned int lcore_id);
    void stop(void);
    void status(OdinData::IpcMessage& status, const std::string& path);
    bool connect(void);
    void configure(OdinData::IpcMessage& config);
    void execute(const std::string& command, OdinData::IpcMessage& reply) override;
    void requestConfiguration(OdinData::IpcMessage& reply);
    std::vector<std::pair<std::string, int>> requestCommands() override;

private:
    // Per-port TX state: the ring fed by the generator plus counters for that port.
    struct TxDeviceContext {
        uint16_t port_id;
        rte_ring* ring;
        uint64_t packets_sent; //!< Accepted by rte_eth_tx_burst on this port
        uint64_t packets_dropped; //!< Rejected by rte_eth_tx_burst and freed

        TxDeviceContext() :
            port_id(UINT16_MAX),
            ring(nullptr),
            packets_sent(0),
            packets_dropped(0)
        {
        }
    };

    PacketTxConfiguration config_;

    int proc_idx_;
    PacketProtocolDecoder* decoder_;
    FrameCallback& frame_callback_;
    DpdkSharedBuffer* shared_buf_;

    struct rte_ring* frame_ready_ring_;
    struct rte_ring* clear_frames_ring_;
    struct rte_ring* upstream_ring_;

    std::vector<TxDeviceContext> tx_devices_;

    uint64_t total_packets_; //!< Packets dequeued from the TX rings
    uint64_t sent_packets_; //!< Packets accepted by the NIC TX queue
    uint64_t dropped_packets_; //!< Packets the NIC TX queue rejected (freed here)

    // Per-burst processing timing, aggregated over rolling 1-second windows in run(), same
    // convention as PacketRxCore. One "burst" is one pass over all TX rings that moved at
    // least one packet.
    double mean_burst_us_; //!< Mean time to process one burst, over the last second
    double min_burst_us_;
    double max_burst_us_; //!< Max time to process one burst, over the last second
    double max_burst_us_all_time_; //!< All-time high-water mark for a single burst's processing time
    double mean_pkts_per_burst_; //!< Mean packets per burst, over the last second
    uint64_t max_pkts_per_burst_; //!< Max packets in a single burst, over the last second
    uint64_t packets_per_second_;
    double busy_percentage_;
    uint64_t estimated_pps_; //!< Sustained throughput capacity, from mean burst time
    uint64_t max_estimated_pps_; //!< All-time high-water mark for estimated_pps_

    std::string core_status_; //!< Remove if DpdkWorkerCore already provides core_status_

    LoggerPtr logger_;
};
}

#endif // INCLUDE_PacketTxCore_H_