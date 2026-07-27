#ifndef INCLUDE_PacketGeneratorCore_H_
#define INCLUDE_PacketGeneratorCore_H_

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
#include <memory>

namespace FrameProcessor
{
    class DataSource;

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
        std::vector<std::string> requestCommands();

    private:

        struct TxDeviceContext
        {
            uint16_t port_id;
            DpdkDevice* device;
            rte_ring* ring;
        };

        bool add_device(const std::string& pci_address);
        int proc_idx_;
        PacketProtocolDecoder* decoder_;
        PacketGeneratorConfiguration config_;

        LoggerPtr logger_;
        FrameCallback& frame_callback_;

        std::vector<TxDeviceContext> tx_devices_;

        struct rte_ring* frame_ready_ring_;
        struct rte_ring* clear_frames_ring_;
        struct rte_ring* upstream_ring_;


        bool packet_tx_;

        DpdkSharedBuffer* shared_buf_;

        std::string instance_pcie_device_;  //!< PCIe address for this instance's NIC
        std::string instance_device_ip_;    //!< IP address for this instance's NIC

        std::unique_ptr<DataSource> data_source_;
    };
}



#endif // INCLUDE_PacketGeneratorCore_H_