#ifndef INCLUDE_FrameWrapperCore_H_
#define INCLUDE_FrameWrapperCore_H_

#include <log4cxx/logger.h>
using namespace log4cxx;
using namespace log4cxx::helpers;
#include <DebugLevelLogger.h>

#include "DpdkWorkerCore.h"
#include "DpdkCoreConfiguration.h"
#include "FrameWrapperCoreConfiguration.h"
#include "ProtocolDecoder.h"
#include <rte_ring.h>
#include <blosc.h>

namespace FrameProcessor
{

    class FrameWrapperCore : public DpdkWorkerCore
    {
    public:

        FrameWrapperCore(
            int fb_idx, int socket_id, DpdkWorkCoreReferences &dpdkWorkCoreReferences
        );
        ~FrameWrapperCore();

        bool run(unsigned int lcore_id);
        void stop(void);
        void status(OdinData::IpcMessage& status, const std::string& path);
        bool connect(void);

    private:
        int proc_idx_;
        ProtocolDecoder* decoder_;
        FrameWrapperConfiguration config_;

        LoggerPtr logger_;
        FrameCallback& frame_callback_;

        uint64_t frames_wrapped_;
        uint64_t frames_wrapped_hz_;
        uint64_t idle_loops_;
        uint64_t avg_us_spent_wrapping_;

        struct rte_ring* frame_ready_ring_;
        struct rte_ring* clear_frames_ring_;
        struct rte_ring* upstream_ring_;
    };
}

#endif // INCLUDE_FrameWrapperCore_H_