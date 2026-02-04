#ifndef INCLUDE_TENSORSTORECSVLOGGER_H_
#define INCLUDE_TENSORSTORECSVLOGGER_H_

#include <fstream>
#include <string>
#include <cstdint>
#include <log4cxx/logger.h>

namespace FrameProcessor {

// CSV logger to write performance metrics.
class TensorstoreCSVLogger {
public:
    TensorstoreCSVLogger();
    ~TensorstoreCSVLogger();
    
    // Opens a CSV log file to write to
    bool Open(const std::string& filename, log4cxx::LoggerPtr logger);
    
    // Logs a write operation to the CSV file
    // Records performance metrics for a completed write operation such as
    // timestamp, frame number, write time, and success status
    void LogWrite(
        uint64_t frame_number,
        size_t num_frames,
        uint64_t write_time_us,
        bool success,
        uint64_t frames_written,
        uint64_t avg_write_time_us,
        unsigned int lcore_id,
        unsigned int frames_per_second,
        uint64_t first_write_time,
        uint64_t tsc_hz
    );
    
    // Closes the CSV log file
    void Close(log4cxx::LoggerPtr logger);
    
    // Checks if logging is currently enabled
    bool IsEnabled() const { return enabled_; }
    
    // Gets the current CSV file path.
    std::string GetPath() const { return path_; }

private:
    std::ofstream file_;  // CSV file output stream
    std::string path_;    // Path to CSV file
    bool enabled_;        // Flag indicating if logging is active
};

} // namespace FrameProcessor

#endif // INCLUDE_TENSORSTORECSVLOGGER_H_
