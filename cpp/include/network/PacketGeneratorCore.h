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

namespace FrameProcessor
{

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

        bool add_device(const std::string& pci_address);
        int proc_idx_;
        PacketProtocolDecoder* decoder_;
        PacketGeneratorConfiguration config_;

        LoggerPtr logger_;
        FrameCallback& frame_callback_;

        // DpdkDevice* device_;
        // uint16_t port_id_;
        // bool packet_tx_ = false;

        // std::vector<DpdkDevice*> devices_;
        // std::unordered_map<uint16_t, rte_ring*> tx_rings_;
        // std::vector<uint16_t> port_ids_;

        struct TxDeviceContext
        {
            uint16_t port_id;
            DpdkDevice* device;
            rte_ring* ring;
        };

        std::vector<TxDeviceContext> tx_devices_;

        struct rte_ring* frame_ready_ring_;
        struct rte_ring* clear_frames_ring_;
        struct rte_ring* upstream_ring_;

        // bool device_configured_;

        bool packet_tx_;

        DpdkSharedBuffer* shared_buf_;

        std::string instance_pcie_device_;  //!< PCIe address for this instance's NIC
        std::string instance_device_ip_;    //!< IP address for this instance's NIC
    };
}



#endif // INCLUDE_PacketGeneratorCore_H_