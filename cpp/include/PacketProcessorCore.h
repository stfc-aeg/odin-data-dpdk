#ifndef INCLUDE_PACKETPROCESSORCORE_H_
#define INCLUDE_PACKETPROCESSORCORE_H_

#include <vector>

#include <log4cxx/logger.h>
using namespace log4cxx;
using namespace log4cxx::helpers;
#include <DebugLevelLogger.h>

#include "DpdkWorkerCore.h"
#include "DpdkSharedBuffer.h"
#include "DpdkCoreConfiguration.h"
#include "PacketProcessorConfiguration.h"
#include "ProtocolDecoder.h"
#include <rte_ring.h>


namespace FrameProcessor
{
    class PacketProcessorCore : public DpdkWorkerCore
    {
    public:
        PacketProcessorCore(
            int proc_idx, int socket_id, DpdkWorkCoreReferences dpdkWorkCoreReferences
        );
        ~PacketProcessorCore();

        bool run(unsigned int lcore_id);
        void stop(void);
        void status(OdinData::IpcMessage& status, const std::string& path);
        bool connect(void);
        void configure(OdinData::IpcMessage& config);

    private:

        int proc_idx_;
        ProtocolDecoder* decoder_;
        DpdkSharedBuffer* shared_buf_;

        PacketProcessorConfiguration config_;

        LoggerPtr logger_;

        
        
        int64_t current_frame_;
        
        uint64_t total_packets_;
        uint64_t packets_hz_;
        uint64_t dropped_packets_;
        uint64_t last_frame_;
        uint64_t processed_frames_;
        uint64_t processed_frames_hz_;
        uint64_t dropped_frames_;
        uint64_t incomplete_frames_;
        uint64_t idle_loops_;
        uint64_t mean_us_on_frame_;
        uint64_t maximum_us_on_frame_;
        uint8_t  core_usage_;


        uint64_t frame_buffer_size_;


        int64_t first_frame_number_;

        struct rte_ring* packet_fwd_ring_;
        struct rte_ring* packet_release_ring_;
        struct rte_ring* clear_frames_ring_;
        std::vector<struct rte_ring*> downstream_rings_;
    };
}
#endif // INCLUDE_PACKETPROCESSORCORE_H_