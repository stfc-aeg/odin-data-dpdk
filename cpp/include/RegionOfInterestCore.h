#ifndef INCLUDE_REGIONOFINTERESTCORE_H_
#define INCLUDE_REGIONOFINTERESTCORE_H_

#include <log4cxx/logger.h>
using namespace log4cxx;
using namespace log4cxx::helpers;
#include <DebugLevelLogger.h>

#include "DpdkWorkerCore.h"
#include "DpdkCoreConfiguration.h"
#include "RegionOfInterestConfiguration.h"
#include "ProtocolDecoder.h"
#include "DpdkSharedBuffer.h"
#include <rte_ring.h>

namespace FrameProcessor
{

    class RegionOfInterestCore : public DpdkWorkerCore
    {
    public:

        RegionOfInterestCore(
            int fb_idx, int socket_id, DpdkWorkCoreReferences &dpdkWorkCoreReferences
        );
        ~RegionOfInterestCore();

        bool run(unsigned int lcore_id);
        void stop(void);
        void status(OdinData::IpcMessage& status, const std::string& path);
        bool connect(void);
        void configure(OdinData::IpcMessage& config);
        void requestConfiguration(OdinData::IpcMessage& reply);

    private:
        int proc_idx_;
        ProtocolDecoder* decoder_;
        std::string mode_;
        DpdkSharedBuffer* shared_buf_;
        RegionOfInterestConfiguration config_;

        LoggerPtr logger_;

        // Status reporting variables
        uint64_t last_frame_;
        uint64_t processed_frames_;
        uint64_t processed_frames_hz_;
        uint64_t idle_loops_;
        uint64_t mean_us_on_frame_;
        uint64_t maximum_us_on_frame_;
        uint8_t core_usage_;

        struct rte_ring* upstream_ring_;
        struct rte_ring* clear_frames_ring_;
        std::vector<struct rte_ring*> downstream_rings_;
    };
}

#endif // INCLUDE_REGIONOFINTERESTCORE_H_
