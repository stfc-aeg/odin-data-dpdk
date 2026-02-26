#ifndef INCLUDE_cameraCAPTURECORE_H_
#define INCLUDE_cameraCAPTURECORE_H_

#include <vector>

#include <log4cxx/logger.h>
using namespace log4cxx;
using namespace log4cxx::helpers;
#include <DebugLevelLogger.h>


#include "DpdkWorkerCore.h"
#include "DpdkSharedBuffer.h"
#include "DpdkCoreConfiguration.h"
#include "camera/CameraCaptureCoreConfiguration.h"
#include "ProtocolDecoder.h"
#include "camera/CameraController.h"
#include "DpdkCamera.h"
#include "camera/DpdkCameraConfiguration.h"
#include "camera/DpdkCameraStatus.h"

#include <rte_ring.h>

namespace FrameProcessor
{
    class CameraCaptureCore : public DpdkWorkerCore
    {
    public:
        CameraCaptureCore(
            int proc_idx, int socket_id, DpdkWorkCoreReferences dpdkWorkCoreReferences
        );
        ~CameraCaptureCore();

        bool run(unsigned int lcore_id);
        void stop(void);
        void status(OdinData::IpcMessage& status, const std::string& path);
        bool connect(void);
        void configure(OdinData::IpcMessage& config);
        void* pop_empty_buffer(void);
        void push_empty_buffer(void* buffer);

    private:

        int proc_idx_;
        ProtocolDecoder* decoder_;
        DpdkSharedBuffer* shared_buf_;

        CameraCaptureCoreConfiguration config_;
        LoggerPtr logger_;
        
        CameraController* camera_controller_;

        bool camera_property_update_;
        bool in_capture_;


        struct rte_ring* clear_frames_ring_;
        std::vector<struct rte_ring*> downstream_rings_;
    };
}
#endif // INCLUDE_cameraCAPTURECORE_H_