#ifndef INCLUDE_TENSORSTORERESIZER_H_
#define INCLUDE_TENSORSTORERESIZER_H_

#include <tensorstore/tensorstore.h>
#include <tensorstore/index.h>
#include <log4cxx/logger.h>
#include <array>
#include <cstdint>

namespace FrameProcessor {

// Manages tensorstore dataset resizing operations.
class TensorstoreResizer {
public:
    // Expands a tensorstore dataset to accommodate more frames
    static uint64_t ExpandDataset(
        tensorstore::TensorStore<>& store,
        uint64_t current_capacity,
        uint64_t expansion_increment,
        tensorstore::Index height,
        tensorstore::Index width,
        log4cxx::LoggerPtr logger
    );

    // Shrinks a tensorstore dataset to final size
    static bool ShrinkDataset(
        tensorstore::TensorStore<>& store,
        uint64_t current_capacity,
        uint64_t final_size,
        tensorstore::Index height,
        tensorstore::Index width,
        log4cxx::LoggerPtr logger
    );

    // Checks if dataset needs expansion for a given frame number
    static inline bool NeedsExpansion(uint64_t frame_number, uint64_t current_capacity) {
        return frame_number >= current_capacity;
    }
};

} // namespace FrameProcessor

#endif // INCLUDE_TENSORSTORERESIZER_H_
