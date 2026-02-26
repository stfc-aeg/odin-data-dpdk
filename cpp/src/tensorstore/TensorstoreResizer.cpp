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
    
    LOG4CXX_DEBUG_LEVEL(2, logger, "Expanding dataset from " << current_capacity 
        << " to " << new_capacity << " frames...");
    
    uint64_t resize_start = rte_get_tsc_cycles();
    
    std::array<tensorstore::Index, 3> inclusive_min = {tensorstore::kImplicit, tensorstore::kImplicit, tensorstore::kImplicit};
    std::array<tensorstore::Index, 3> exclusive_max = {new_capacity, tensorstore::kImplicit, tensorstore::kImplicit};
    
    auto resize_result = tensorstore::Resize(
        store,
        inclusive_min,
        exclusive_max,
        tensorstore::expand_only
    ).result();
    
    if (!resize_result.ok()) {
        LOG4CXX_ERROR(logger, "Failed to expand dataset: " << resize_result.status());
        // Return current capacity unchanged to prevent invalid state
        return current_capacity;
    }
    
    store = *resize_result;
    
    uint64_t resize_end = rte_get_tsc_cycles();
    uint64_t resize_time_us = ((resize_end - resize_start) * 1000000) / rte_get_tsc_hz();
    LOG4CXX_INFO(logger, "Dataset expanded successfully to " << new_capacity 
        << " frames in " << resize_time_us << " us");
    
    return new_capacity;
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
    
    uint64_t resize_start = rte_get_tsc_cycles();
    
    std::array<tensorstore::Index, 3> inclusive_min = {tensorstore::kImplicit, tensorstore::kImplicit, tensorstore::kImplicit};
    std::array<tensorstore::Index, 3> exclusive_max = {static_cast<tensorstore::Index>(final_size), tensorstore::kImplicit, tensorstore::kImplicit};
    
    auto resize_result = tensorstore::Resize(
        store,
        inclusive_min,
        exclusive_max,
        tensorstore::shrink_only
    ).result();
    
    if (!resize_result.ok()) {
        LOG4CXX_ERROR(logger, "Failed to shrink dataset: " << resize_result.status());
        return false;
    }
    
    store = *resize_result;
    
    uint64_t resize_end = rte_get_tsc_cycles();
    uint64_t resize_time_us = ((resize_end - resize_start) * 1000000) / rte_get_tsc_hz();
    LOG4CXX_INFO(logger, "Dataset shrunk successfully to " << final_size 
        << " frames in " << resize_time_us << " us");
    
    return true;
}

} // namespace FrameProcessor
