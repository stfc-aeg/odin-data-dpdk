#include "TensorstorePerformanceMonitor.h"
#include <rte_cycles.h>

namespace FrameProcessor {

TensorstorePerformanceMonitor::TensorstorePerformanceMonitor()
    : frames_this_second_(1),
      total_frame_cycles_(1),
      cycles_working_(1),
      maximum_frame_cycles_(0),
      idle_loops_counter_(0),
      last_update_cycles_(rte_get_tsc_cycles()),
      frames_per_second_(0),
      mean_us_on_frame_(0),
      maximum_us_on_frame_(0),
      idle_loops_(0),
      core_usage_(0)
{
}

void TensorstorePerformanceMonitor::UpdateStatistics(uint64_t cycles_per_sec)
{
    // Calculates various write statistics
    frames_per_second_ = frames_this_second_ - 1;
    mean_us_on_frame_ = (total_frame_cycles_ * 1000000) / (frames_this_second_ * cycles_per_sec);
    core_usage_ = (cycles_working_ * 255) / cycles_per_sec;
    maximum_us_on_frame_ = (maximum_frame_cycles_ * 1000000) / cycles_per_sec;
    idle_loops_ = idle_loops_counter_;

    // Resets counters for the next second
    frames_this_second_ = 1;
    idle_loops_counter_ = 0;
    total_frame_cycles_ = 1;
    cycles_working_ = 1;
    maximum_frame_cycles_ = 0;
    last_update_cycles_ = rte_get_tsc_cycles();
}

void TensorstorePerformanceMonitor::RecordFrameProcessing(uint64_t cycles_spent)
{
    total_frame_cycles_ += cycles_spent;
    cycles_working_ += cycles_spent;
    
    if (maximum_frame_cycles_ < cycles_spent) {
        maximum_frame_cycles_ = cycles_spent;
    }
    
    frames_this_second_++;
}

void TensorstorePerformanceMonitor::RecordIdleLoop()
{
    idle_loops_counter_++;
}

bool TensorstorePerformanceMonitor::ShouldUpdate(
    uint64_t current_cycles,
    uint64_t cycles_per_sec) const
{
    return (current_cycles - last_update_cycles_) >= cycles_per_sec;
}

} // namespace FrameProcessor
