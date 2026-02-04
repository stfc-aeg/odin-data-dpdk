#ifndef INCLUDE_TENSORSTORECORE_H_
#define INCLUDE_TENSORSTORECORE_H_

// Standard library
#include <atomic>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

// Logging
#include <log4cxx/logger.h>
#include <DebugLevelLogger.h>

// TensorStore
#include <tensorstore/tensorstore.h>
#include <tensorstore/util/future.h>

#include "DpdkWorkerCore.h"
#include "DpdkCoreConfiguration.h"
#include "TensorstoreCoreConfiguration.h"
#include "ProtocolDecoder.h"
#include "DpdkSharedBuffer.h"

// DPDK
#include <rte_ring.h>

// TensorStore utility modules
#include "TensorstoreCSVLogger.h"
#include "TensorstorePerformanceMonitor.h"

namespace FrameProcessor
{
    // Enumeration of supported pixel data types.
    enum class PixelDataType {
        UINT8,  // 8-bit unsigned integer (1 byte)
        UINT16,  // 16-bit unsigned integer (2 bytes)
        UINT32,  // 32-bit unsigned integer (4 bytes)
        UINT64   // 64-bit unsigned integer (8 bytes)
    };

    // DPDK worker core for asynchronous TensorStore write operations.
    class TensorstoreCore : public DpdkWorkerCore
    {
    public:
        TensorstoreCore(
            int fb_idx, 
            int socket_id, 
            DpdkWorkCoreReferences &dpdkWorkCoreReferences
        );

        ~TensorstoreCore();

        // Main work loop running on dedicated CPU core
        bool run(unsigned int lcore_id);

        // Stops the worker core and cleans up resources
        void stop(void);

        // Reports performance statistics and status
        void status(OdinData::IpcMessage& status, const std::string& path);

        // Connects to upstream input rings
        bool connect(void);

        // Applies runtime configuration updates
        void configure(OdinData::IpcMessage& config);

    private:
        
        // Tracks information for an in-progress asynchronous write
        struct PendingWrite
        {
            uint64_t frame_number;                       // First frame number in this write
            std::vector<::SuperFrameHeader*> frame_buffers; // Frame buffer pointers
            tensorstore::WriteFutures write_future;      // TensorStore write future
            uint64_t start_cycles;                       // TSC timestamp when write started
        };

        // Checks for and processes completed write operations
        void pollAndProcessCompletions();

        // Forwards a frame buffer to downstream ring
        void forwardFrame(::SuperFrameHeader* frame_buffer, uint64_t frame_number);

        // Handles dataset reconfiguration
        // Flushes pending writes, closes existing dataset, and creates
        // a new dataset with updated configuration
        void handleReconfiguration();

        // Core configuration
        int proc_idx_;
        log4cxx::LoggerPtr logger_;
        TensorstoreCoreConfiguration config_;
        
        // DPDK resources
        ProtocolDecoder* decoder_;
        DpdkSharedBuffer* shared_buf_;
        struct rte_ring* clear_frames_ring_;
        struct rte_ring* upstream_ring_;
        std::vector<struct rte_ring*> downstream_rings_;
        
        // TensorStore state
        bool tensorstore_initialized_;
        std::string data_type_;
        PixelDataType pixel_type_;
        std::optional<tensorstore::TensorStore<>> store_;
        uint64_t current_dataset_capacity_;
        uint64_t highest_frame_written_;
        std::string last_error_message_;
        bool flush_pending_writes;
        
        // Write tracking
        std::unordered_map<uint64_t, PendingWrite> pending_writes_queue_;
        std::vector<struct SuperFrameHeader*> frame_chunk_buffer_;
        
        // Performance statistics
        uint64_t last_frame_;
        uint64_t processed_frames_;
        TensorstorePerformanceMonitor perf_monitor_;
        
        // Write statistics
        uint64_t frames_written_;
        uint64_t write_errors_;
        uint64_t avg_write_time_us_;
        uint64_t pending_writes_count_;
        uint64_t frames_forwarded_;
        uint64_t completed_writes_;
        
        // CSV logging
        TensorstoreCSVLogger csv_logger_;
        std::string csv_path_;
        uint64_t run_start_time_;
        uint64_t first_write_time_;
        bool first_write_recorded_;
        unsigned int frames_per_second_;
    };

} // namespace FrameProcessor

#endif // INCLUDE_TENSORSTORECORE_H_