#ifndef INCLUDE_PacketGeneratorCore_H_
#define INCLUDE_PacketGeneratorCore_H_

#include <cstdint>
#include <memory>
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
#include "PacketGeneratorCoreConfiguration.h"
#include "PacketProtocolDecoder.h"
#include <rte_ring.h>
#include <blosc.h>
#include <rte_byteorder.h>

namespace FrameProcessor
{
    class DataSource;

    enum class RoundRobinMode
    {
        Frame,
        Packet
    };

    class PacketGeneratorCore : public DpdkWorkerCore
    {
    public:

        PacketGeneratorCore(
            int fb_idx, int socket_id, DpdkWorkCoreReferences &dpdkWorkCoreReferences
        );
        ~PacketGeneratorCore();

        bool run(unsigned int lcore_id);
        void stop(void);
        void status(OdinData::IpcMessage& status, const std::string& path);
        bool connect(void);
        void configure(OdinData::IpcMessage& config);
        void execute(const std::string& command, OdinData::IpcMessage& reply);
        void start_tx(void);
        void stop_tx(void);
        void requestConfiguration(OdinData::IpcMessage& reply);
        std::vector<std::pair<std::string, int>> requestCommands() override;

    private:

        struct TxDeviceContext
        {
            uint16_t port_id;
            DpdkDevice* device;
            rte_ring* ring;
        };

        bool add_device(const std::string& pci_address);

        PacketGeneratorConfiguration config_;

        int proc_idx_;
        PacketProtocolDecoder* decoder_;
        FrameCallback& frame_callback_;
        DpdkSharedBuffer* shared_buf_;
        boost::shared_ptr<DataSource> data_source_;

        RoundRobinMode round_robin_mode_;
        std::vector<TxDeviceContext> tx_devices_;

        struct rte_ring* frame_ready_ring_;
        struct rte_ring* clear_frames_ring_;
        struct rte_ring* upstream_ring_;

        bool packet_tx_;

        uint64_t total_packets_;          //!< Every packet slot considered (sent + dropped + alloc failures)
        uint64_t packets_sent_;           //!< Packets enqueued onto a TX ring
        uint64_t dropped_packets_;        //!< Packets deliberately dropped via the packet_drop config
        uint64_t mbuf_alloc_failures_;    //!< Packets lost because no mbuf could be allocated
        uint64_t tx_ring_full_waits_;     //!< Spins waiting on a full TX ring (downstream backpressure)
        uint64_t frames_generated_;
        uint64_t last_frame_number_;

        // Per-frame processing timing, aggregated over rolling 1-second windows in run(), using
        // the same convention as PacketRxCore (which does it per rx_burst). One generated frame
        // is the unit of work here. Includes time spent waiting on a full TX ring.
        double mean_frame_us_;            //!< Mean time to generate and enqueue one frame, over the last second
        double min_frame_us_;
        double max_frame_us_;             //!< Max time for a single frame, over the last second
        double max_frame_us_all_time_;    //!< All-time high-water mark for a single frame's processing time
        uint64_t packets_per_second_;
        uint64_t frames_per_second_;
        double busy_percentage_;

        std::string core_status_;         //!< Remove if DpdkWorkerCore already provides core_status_

        LoggerPtr logger_;
    };
}

#endif // INCLUDE_PacketGeneratorCore_H_