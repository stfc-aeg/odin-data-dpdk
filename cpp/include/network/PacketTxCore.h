#ifndef INCLUDE_PacketTxCore_H_
#define INCLUDE_PacketTxCore_H_

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

namespace FrameProcessor
{

    class PacketTxCore : public DpdkWorkerCore
    {
    public:

        PacketTxCore(
            int fb_idx, int socket_id, DpdkWorkCoreReferences &dpdkWorkCoreReferences
        );
        ~PacketTxCore();

        bool run(unsigned int lcore_id);
        void stop(void);
        void status(OdinData::IpcMessage& status, const std::string& path);
        bool connect(void);
        void configure(OdinData::IpcMessage& config);
        // void execute(const std::string& command, OdinData::IpcMessage& reply);
        // std::vector<std::string> requestCommands();

    private:

        int proc_idx_;
        PacketProtocolDecoder* decoder_;
        PacketTxConfiguration config_;

        LoggerPtr logger_;
        FrameCallback& frame_callback_;

        struct rte_ring* frame_ready_ring_;
        struct rte_ring* clear_frames_ring_;
        struct rte_ring* upstream_ring_;

        struct TxDeviceContext
        {
            uint16_t port_id;
            rte_ring* ring;
        };
        std::vector<TxDeviceContext> tx_devices_;

        DpdkSharedBuffer* shared_buf_;
    };
}



#endif // INCLUDE_PacketTxCore_H_