#include "TensorstoreResizer.h"
#include <DebugLevelLogger.h>
#include <rte_cycles.h>

namespace FrameProcessor {

uint64_t TensorstoreResizer::ExpandDataset(
    tensorstore::TensorStore<>& store,
    uint64_t current_capacity,
    uint64_t expansion_increment,
    tensorstore::Index height,
    tensorstore::Index width,
    log4cxx::LoggerPtr logger)
{
    tensorstore::Index new_capacity = current_capacity + expansion_increment;
    
    std::array<tensorstore::Index, 3> inclusive_min = {0, 0, 0};
    std::array<tensorstore::Index, 3> exclusive_max = {new_capacity, height, width};
    
    tensorstore::span<const tensorstore::Index> inclusive_min_span(
        inclusive_min.data(), inclusive_min.size()
    );
    tensorstore::span<const tensorstore::Index> exclusive_max_span(
        exclusive_max.data(), exclusive_max.size()
    );
    
    LOG4CXX_DEBUG_LEVEL(2, logger, "Expanding dataset from " << current_capacity 
        << " to " << new_capacity << " frames...");
    
    uint64_t resize_start = rte_get_tsc_cycles();
    
    auto resize_status = tensorstore::Resize(
        store,
        inclusive_min_span,
        exclusive_max_span,
        tensorstore::ResizeMode::expand_only
    ).result();
    
    
    if (!resize_status.ok()) {
        LOG4CXX_ERROR(logger, "Failed to expand dataset");
        // Return current capacity unchanged to prevent invalid state
        return current_capacity;
    } else {
        LOG4CXX_INFO(logger, "Dataset expanded successfully");
        return new_capacity;
    }
}

// Shrinks the dataset to its actual size to remove empty spaces
bool TensorstoreResizer::ShrinkDataset(
    tensorstore::TensorStore<>& store,
    uint64_t current_capacity,
    uint64_t final_size,
    tensorstore::Index height,
    tensorstore::Index width,
    log4cxx::LoggerPtr logger)
{
    if (final_size >= current_capacity) {
        LOG4CXX_DEBUG_LEVEL(2, logger, "No shrinking needed: final_size=" << final_size 
            << " >= current_capacity=" << current_capacity);
        return true;
    }
    
    LOG4CXX_DEBUG_LEVEL(2, logger, "Shrinking dataset from " << current_capacity 
        << " to " << final_size << " frames...");
    
    std::array<tensorstore::Index, 3> inclusive_min = {0, 0, 0};
    std::array<tensorstore::Index, 3> exclusive_max = {
        static_cast<tensorstore::Index>(final_size), height, width
    };
    
    tensorstore::span<const tensorstore::Index> inclusive_min_span(
        inclusive_min.data(), inclusive_min.size()
    );
    tensorstore::span<const tensorstore::Index> exclusive_max_span(
        exclusive_max.data(), exclusive_max.size()
    );
    
    uint64_t resize_start = rte_get_tsc_cycles();
    
    auto resize_status = tensorstore::Resize(
        store,
        inclusive_min_span,
        exclusive_max_span,
        tensorstore::ResizeMode::resize_metadata_only | tensorstore::ResizeMode::shrink_only
    ).result();
    
    
    if (!resize_status.ok()) {
        LOG4CXX_ERROR(logger, "Failed to shrink dataset");
        return false;
    } else {
        LOG4CXX_INFO(logger, "Dataset shrunk successfully");
        return true;
    }
}

} // namespace FrameProcessor
