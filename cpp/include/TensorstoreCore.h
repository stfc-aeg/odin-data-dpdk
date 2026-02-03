////////////////////////////////////////////////////////////////////////////////
// SECTION 1: HEADER GUARD
////////////////////////////////////////////////////////////////////////////////
#ifndef INCLUDE_TENSORSTORECORE_H_
#define INCLUDE_TENSORSTORECORE_H_

////////////////////////////////////////////////////////////////////////////////
// SECTION 2: INCLUDES (DEPENDENCIES)
////////////////////////////////////////////////////////////////////////////////

#include <atomic> // For thread-safe atomic variables (like counters).
#include <deque>  // For managing the queue of pending asynchronous writes.
#include <fstream> // For CSV file output

#include <log4cxx/logger.h> // For the logging system.
using namespace log4cxx;
using namespace log4cxx::helpers;
#include <DebugLevelLogger.h>

#include "DpdkWorkerCore.h" // Includes the "base class" that this class builds upon.
#include "DpdkCoreConfiguration.h"
#include "TensorstoreCoreConfiguration.h" // The specific configuration class for this core.
#include "ProtocolDecoder.h" // A helper class for reading frame metadata.
#include "DpdkSharedBuffer.h" // For managing shared memory.
#include <rte_ring.h> // For DPDK's high-speed "mailboxes" (rings).

// TensorStore includes
#include <tensorstore/tensorstore.h>
#include <tensorstore/context.h>
#include <tensorstore/array.h>
#include <tensorstore/data_type.h>
#include <tensorstore/util/future.h> // For tensorstore::Future
#include <optional> // For std::optional

////////////////////////////////////////////////////////////////////////////////
// SECTION 3: NAMESPACE AND CLASS DECLARATION
////////////////////////////////////////////////////////////////////////////////
namespace FrameProcessor
{
    // Enum for pixel data types
    enum class PixelDataType {
        UINT8,
        UINT16,
        UINT32,
        UINT64
    };

    /**
     * This class is a DPDK "worker" responsible for taking
     * image frames, initializing a TensorStore (a large-scale data
     * storage), and *asynchronously* writing those frames into the store.
     *
     * It inherits from `DpdkWorkerCore`, meaning it *is* a DpdkWorkerCore
     * and must provide the functions that "contract" requires (like run,
     * stop, status, etc.).
     */
    class TensorstoreCore : public DpdkWorkerCore
    {
    // --- PUBLIC INTERFACE ---
    public:
        /**
         * Constructor
         */
        TensorstoreCore(
            int fb_idx, int socket_id, DpdkWorkCoreReferences &dpdkWorkCoreReferences
        );

        /**
         * Destructor
         */
        ~TensorstoreCore();

        /**
         * The main work loop that runs on a dedicated CPU core.
         */
        bool run(unsigned int lcore_id);

        /**
         * Called from another thread to tell the `run` loop to exit.
         */
        void stop(void);

        /**
         * Gathers all performance statistics and reports them.
         */
        void status(OdinData::IpcMessage& status, const std::string& path);

        /**
         * Connects this core to the *input* rings from other cores.
         */
        bool connect(void);

        /**
         * Called to send new configuration settings *while*
         * the core is running.
         */
        void configure(OdinData::IpcMessage& config);

    // --- PRIVATE MEMBERS ---
    private:
        
        /**
         * Tracks all information needed for an in-progress async write.
         */
        struct PendingWrite
        {
            uint64_t frame_number;                 // First frame number in chunk
            std::vector<struct SuperFrameHeader*> frame_buffers; // Pointers to all frame buffers in chunk
            tensorstore::WriteFutures write_future;
            uint64_t start_cycles;
        };

        // --- Private Helper Functions ---

        /**
         *Initiates an asynchronous write of a frame to TensorStore.
         */
        template <typename T>
        tensorstore::WriteFutures asyncWriteFrame(
            tensorstore::TensorStore<>& store,
            void* raw_data,
            tensorstore::Index height,
            tensorstore::Index width,
            uint64_t frame_number
        );

        /**
         * Initiates an asynchronous write of multiple frames to TensorStore.
         */
        template <typename T>
        tensorstore::WriteFutures asyncWriteFrameChunk(
            tensorstore::TensorStore<>& store,
            const std::vector<void*>& raw_data_ptrs,
            tensorstore::Index height,
            tensorstore::Index width,
            uint64_t start_frame_number,
            size_t num_frames
        );

        /**
         * Checks the front of the queue to see if the oldest
         * write has completed. If so, processes it, forwards the frame,
         * and removes it from the queue.
         */
        void pollAndProcessCompletions();

        /**
         * Forwards a completed frame buffer downstream.
         */
        void forwardFrame(struct SuperFrameHeader* frame_buffer, uint64_t frame_number);

        /**
         * Handles closing the old store and creating a new one.
         */
        void handleReconfiguration();
        
        /**
         * Opens a CSV log file
         */
        void openCSVLog(const std::string& filename);
        
        /**
         * Logs a write operation to the CSV file
         */
        void logWriteToCSV(uint64_t frame_number, size_t num_frames, 
                          uint64_t write_time_us, bool success);
        
        /**
         * Closes the CSV log file
         */
        void closeCSVLog(); 

        // --- Core Identity and Helpers ---
        int proc_idx_; // This core's unique ID.
        ProtocolDecoder* decoder_; // Pointer to the helper that reads frame data.
        DpdkSharedBuffer* shared_buf_; // Pointer to the shared memory manager.
        TensorstoreCoreConfiguration config_; // Holds settings loaded from a file.
        LoggerPtr logger_; // The logging object.
        
        // --- Data Type Management ---
        PixelDataType pixel_type_; // Current pixel data type based on bit depth
        
        // --- Status Reporting Variables ---
        uint64_t last_frame_; // The number of the last frame processed.
        uint64_t processed_frames_; // Total frames dequeued from upstream.
        uint64_t processed_frames_hz_; // Calculated frames per second.
        uint64_t idle_loops_; // How many times the loop ran but had no work.
        uint64_t mean_us_on_frame_; // Average time (in microseconds) spent per frame.
        uint64_t maximum_us_on_frame_; // Max time spent per frame.
        uint8_t core_usage_; // CPU usage (scaled 0-255).
        
        // --- DPDK Rings ---
        struct rte_ring* clear_frames_ring_; // Ring to return used buffers to.
        struct rte_ring* upstream_ring_; // The ring we receive frames from.
        std::vector<struct rte_ring*> downstream_rings_; // List of rings we *send* frames to.
        
        // --- TensorStore Members ---
        bool tensorstore_initialized_; // A flag: true if the store is open and ready.
        std::string storage_path_; // The file path for storage (from config).
        std::string data_type_;    // The type of data (e.g., "uint16").
        size_t total_frames_;
        size_t frame_size_;
        
        // The actual TensorStore object.
        // We use std::optional to delay its construction until the first frame.
        std::optional<tensorstore::TensorStore<>> store_;

        // --- Asynchronous Write Tracking ---
        
        /**
         * This queue stores info for all writes that have been initiated
         * but have not yet completed.
         */
    
        std::unordered_map<uint64_t, PendingWrite> pending_writes_queue_;
        
        // --- Statistics for TensorStore Operations ---
        uint64_t frames_written_; // Total frames successfully written.
        uint64_t write_errors_;   // Total frames that failed to write.
        uint64_t avg_write_time_us_; // Average time taken per write (microseconds).
        
        // Stats for async operations
        uint64_t pending_writes_count_; // Current number of writes in the queue.
        uint64_t frames_forwarded_; // Total frames successfully forwarded downstream.
        uint64_t completed_writes_;        
        // --- CSV Logging Members ---
        std::ofstream csv_file_; // CSV output file stream
        std::string csv_path_; // Path to CSV log file
        bool csv_logging_enabled_; // Flag to enable/disable CSV logging
        uint64_t run_start_time_; // Timestamp when run() started
        uint64_t first_write_time_; // Timestamp of first write (for relative timing)
        bool first_write_recorded_; // Flag to track if first write has been recorded
        unsigned int frames_per_second_; // Frame rate from camera config (send from the odin-gui)
        uint64_t current_dataset_capacity_; // Current size of dataset
        uint64_t highest_frame_written_; // Highest frame number written (for final dataset resize)

        std::vector<struct SuperFrameHeader*> frame_chunk_buffer_; // Buffer for accumulating frames
        std::string last_error_message_; // Store last configuration error for status reporting
        bool enable_writing_; // Flag to enable/disable writing to Tensorstore
        bool flush_pending_writes = false; // Flag to indicate if pending writes should be flushed

    };
} // End of FrameProcessor namespace

#endif // INCLUDE_TENSORSTORECORE_H_