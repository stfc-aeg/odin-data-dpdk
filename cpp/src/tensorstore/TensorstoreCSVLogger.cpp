#include "TensorstoreCSVLogger.h"
#include <DebugLevelLogger.h>
#include <rte_cycles.h>

using namespace log4cxx;
using namespace log4cxx::helpers;

namespace FrameProcessor {

TensorstoreCSVLogger::TensorstoreCSVLogger()
    : enabled_(false)
{
}

TensorstoreCSVLogger::~TensorstoreCSVLogger()
{
    if (file_.is_open()) {
        file_.close();
    }
}

bool TensorstoreCSVLogger::Open(const std::string& filename, log4cxx::LoggerPtr logger)
{
    path_ = filename;
    
    // Check if file exists before opening
    std::ifstream check_file(path_);
    bool file_exists = check_file.good();
    check_file.close();
    
    // Use append mode to support concurrent writes from multiple cores
    file_.open(path_, std::ios::out | std::ios::app);
    
    if (file_.is_open()) {
        enabled_ = true;
        
        // Avoids duplicate headers when multiple cores write to same file
        if (!file_exists) {
            file_ << "timestamp_seconds,frame_number,num_frames,write_time_us,"
                  << "success,cumulative_frames,avg_write_time_us,core_id,frames_per_second\n";
            file_.flush();
        }
        
        return true;
    } else {
        enabled_ = false;
        LOG4CXX_ERROR(logger, "Failed to open CSV log file: " << path_);
        return false;
    }
}

void TensorstoreCSVLogger::LogWrite(
    uint64_t frame_number,
    size_t num_frames,
    uint64_t write_time_us,
    bool success,
    uint64_t frames_written,
    uint64_t avg_write_time_us,
    unsigned int lcore_id,
    unsigned int frames_per_second,
    uint64_t first_write_time,
    uint64_t tsc_hz)
{
    if (!file_.is_open() || !enabled_) {
        return;
    }
    
    // Calculate timestamp in seconds since first write
    uint64_t current_cycles = rte_get_tsc_cycles();
    double elapsed_seconds = static_cast<double>(current_cycles - first_write_time) / 
                            static_cast<double>(tsc_hz);
    
    // Write data row
    file_ << elapsed_seconds << ","
          << frame_number << ","
          << num_frames << ","
          << write_time_us << ","
          << (success ? "1" : "0") << ","
          << frames_written << ","
          << avg_write_time_us << ","
          << lcore_id << ","
          << frames_per_second << "\n";
    file_.flush();
}

void TensorstoreCSVLogger::Close(log4cxx::LoggerPtr logger)
{
    if (file_.is_open()) {
        file_.close();
        LOG4CXX_INFO(logger, "CSV log file closed: " << path_);
        enabled_ = false;
    }
}

} // namespace FrameProcessor
