#include "TensorstoreFlushManager.h"
#include "TensorstoreResizer.h"
#include <DebugLevelLogger.h>

namespace FrameProcessor {

FlushResult TensorstoreFlushManager::FlushPendingWrites(
    std::optional<tensorstore::TensorStore<>>& store,
    bool& tensorstore_initialized,
    uint64_t highest_frame_written,
    uint64_t current_capacity,
    uint64_t frame_height,
    uint64_t frame_width,
    log4cxx::LoggerPtr logger)
{
    FlushResult result = {
        .success = true,
        .frames_forwarded = 0,
        .dataset_shrunk = false
    };

    if (!tensorstore_initialized || !store.has_value()) {
        LOG4CXX_DEBUG_LEVEL(2, logger, "No active store to flush");
        return result;
    }

    // Shrink dataset to actual size once frames have finished writing
    if (highest_frame_written > 0) {
        uint64_t final_size = highest_frame_written + 1;
        
        result.dataset_shrunk = TensorstoreResizer::ShrinkDataset(
            *store,
            current_capacity,
            final_size,
            static_cast<tensorstore::Index>(frame_height),
            static_cast<tensorstore::Index>(frame_width),
            logger
        );
    }

    store.reset();
    tensorstore_initialized = false;

    return result;
}

void TensorstoreFlushManager::ResetWriteStatistics(
    uint64_t& frames_written,
    uint64_t& write_errors,
    uint64_t& current_capacity,
    uint64_t& highest_frame)
{
    frames_written = 0;
    write_errors = 0;
    current_capacity = 0;
    highest_frame = 0;
}

} // namespace FrameProcessor
