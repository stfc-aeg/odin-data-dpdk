#ifndef INCLUDE_TENSORSTOREFLUSHMANAGER_H_
#define INCLUDE_TENSORSTOREFLUSHMANAGER_H_

#include <tensorstore/tensorstore.h>
#include <log4cxx/logger.h>
#include <cstdint>
#include <optional>
#include <vector>

namespace FrameProcessor {

struct SuperFrameHeader;

// Result of a flush operation
struct FlushResult {
    bool success;                
    uint64_t frames_forwarded;    
    bool dataset_shrunk;          
};

// Manages the flushing of pending writes and dataset cleanup
class TensorstoreFlushManager {
public:
    // Performs a complete flush operation.
    // Executes the following sequence:
    // 1. Waits for all pending writes to complete
    // 2. Forwards any buffered frames
    // 3. Shrinks dataset to final size (if applicable)
    // 4. Cleans up tensorstore resources
    static FlushResult FlushPendingWrites(
        std::optional<tensorstore::TensorStore<>>& store,
        bool& tensorstore_initialized,
        uint64_t highest_frame_written,
        uint64_t current_capacity,
        uint64_t frame_height,
        uint64_t frame_width,
        log4cxx::LoggerPtr logger
    );

    // Resets write statistics after a flush has been completed
    static void ResetWriteStatistics(
        uint64_t& frames_written,
        uint64_t& write_errors,
        uint64_t& current_capacity,
        uint64_t& highest_frame
    );
};

} // namespace FrameProcessor

#endif // INCLUDE_TENSORSTOREFLUSHMANAGER_H_
