#ifndef INCLUDE_TENSORSTOREPERFORMANCEMONITOR_H_
#define INCLUDE_TENSORSTOREPERFORMANCEMONITOR_H_

#include <cstdint>

namespace FrameProcessor {

// Performance monitoring and statistics tracking for TensorStore operations
class TensorstorePerformanceMonitor {
public:
    TensorstorePerformanceMonitor();

    // Updates performance statistics every second
    void UpdateStatistics(uint64_t cycles_per_sec);

    // Records frame processing timing
    void RecordFrameProcessing(uint64_t cycles_spent);

    // Records an idle loop iteration
    void RecordIdleLoop();

    // Checks if statistics should be updated
    bool ShouldUpdate(uint64_t current_cycles, uint64_t cycles_per_sec) const;

    // Getters for current statistics
    uint64_t GetFramesPerSecond() const { return frames_per_second_; }
    uint64_t GetMeanFrameTimeUs() const { return mean_us_on_frame_; }
    uint64_t GetMaxFrameTimeUs() const { return maximum_us_on_frame_; }
    uint64_t GetIdleLoops() const { return idle_loops_; }
    uint8_t GetCoreUsage() const { return core_usage_; }

    // Accessors for accumulator counters 
    uint64_t& FramesThisSecond() { return frames_this_second_; }
    uint64_t& CyclesWorking() { return cycles_working_; }
    uint64_t& LastUpdateCycles() { return last_update_cycles_; }

private:
    // Accumulator counters (reset each second)
    uint64_t frames_this_second_;    // Frames processed in current second
    uint64_t total_frame_cycles_;    // Total cycles spent on frames this second
    uint64_t cycles_working_;        // Total working cycles this second
    uint64_t maximum_frame_cycles_;  // Maximum cycles for single frame this second
    uint64_t idle_loops_counter_;   // Idle loops this second
    uint64_t last_update_cycles_;   // TSC value of last statistics update

    // For the write statistics (calculated each second)
    uint64_t frames_per_second_;    // Frames processed per second
    uint64_t mean_us_on_frame_;     // Average frame processing time (us)
    uint64_t maximum_us_on_frame_;  // Maximum frame processing time (us)
    uint64_t idle_loops_;           // Idle loops in last second
    uint8_t core_usage_;            // CPU usage 
};

} // namespace FrameProcessor

#endif // INCLUDE_TENSORSTOREPERFORMANCEMONITOR_H_
