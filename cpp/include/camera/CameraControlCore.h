#ifndef INCLUDE_CameraCONTROLCORE_H_
#define INCLUDE_CameraCONTROLCORE_H_

#include <vector>

#include <log4cxx/logger.h>
using namespace log4cxx;
using namespace log4cxx::helpers;
#include <DebugLevelLogger.h>

#include "DpdkWorkerCore.h"
#include "DpdkSharedBuffer.h"
#include "DpdkCoreConfiguration.h"
#include "camera/CameraControlCoreConfiguration.h"
#include "ProtocolDecoder.h"
#include "camera/CameraController.h"

#include <rte_ring.h>

#include "IpcChannel.h"
#include "IpcMessage.h"
#include <boost/scoped_ptr.hpp>
#include <boost/thread.hpp>

namespace FrameProcessor
{
    class CameraControlCore : public DpdkWorkerCore
    {
    public:
        CameraControlCore(
            int proc_idx, int socket_id, DpdkWorkCoreReferences dpdkWorkCoreReferences
        );
        ~CameraControlCore();

        bool run(unsigned int lcore_id);
        void stop(void);
        void status(OdinData::IpcMessage& status, const std::string& path);
        bool connect(void);
        void configure(OdinData::IpcMessage& config);

    private:

        int proc_idx_;
        ProtocolDecoder* decoder_;
        DpdkSharedBuffer* shared_buf_;

        CameraControlCoreConfiguration config_;
        LoggerPtr logger_;

        OdinData::IpcChannel Camera_Ctrl_Channel_;

        // Control channel request accounting, so status() can show whether the channel is being
        // exercised and whether requests are being rejected.
        uint64_t requests_received_;    //!< Total requests decoded from the control channel
        uint64_t requests_failed_;      //!< Requests answered with a nack
        uint64_t configure_requests_;   //!< MsgValCmdConfigure requests
        uint64_t status_requests_;      //!< MsgValCmdStatus requests
        uint64_t config_requests_;      //!< MsgValCmdRequestConfiguration requests
        std::string last_error_;        //!< Reason the most recent failed request was rejected
        std::string last_client_;       //!< Identity of the most recent requesting client
        bool channel_bound_;            //!< Whether the ZMQ ROUTER socket has been bound

        CameraController* CameraController_;

        struct rte_ring* clear_frames_ring_;
        std::vector<struct rte_ring*> downstream_rings_;
    };
}
#endif // INCLUDE_CameraControlCore_H_