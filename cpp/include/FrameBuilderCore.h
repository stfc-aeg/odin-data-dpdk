#ifndef INCLUDE_FRAMEBUILDERCORE_H_
#define INCLUDE_FRAMEBUILDERCORE_H_

#include <log4cxx/logger.h>
using namespace log4cxx;
using namespace log4cxx::helpers;
#include <DebugLevelLogger.h>

#include "DpdkWorkerCore.h"
#include "DpdkSharedBuffer.h"
#include "DpdkCoreConfiguration.h"
#include "FrameBuilderConfiguration.h"
#include "ProtocolDecoder.h"
#include <rte_ring.h>
#include <blosc.h>

namespace FrameProcessor
{

    class FrameBuilderCore : public DpdkWorkerCore
    {
    public:

        FrameBuilderCore(
            int fb_idx, int socket_id, DpdkWorkCoreReferences &dpdkWorkCoreReferences
        );
        ~FrameBuilderCore();

        bool run(unsigned int lcore_id);
        void stop(void);
        void status(OdinData::IpcMessage& status, const std::string& path);
        bool connect(void);
        void configure(OdinData::IpcMessage& config);

    private:
        int proc_idx_;
        ProtocolDecoder* decoder_;
        DpdkSharedBuffer* shared_buf_;
        FrameBuilderConfiguration config_;

        LoggerPtr logger_;

        uint64_t built_frames_;
        uint64_t built_frames_hz_;
        uint64_t idle_loops_;
        uint64_t avg_us_spent_building_;


        struct rte_ring* upstream_ring;
        struct rte_ring* clear_frames_ring_;
        std::vector<struct rte_ring*> downstream_rings_;
    };
}

#endif // INCLUDE_FRAMEBUILDERCORE_H_