#include "TensorstoreCore.h"

// DPDK includes
#include "DpdkUtils.h"
#include "DpdkSharedBufferFrame.h"
#include "camera/CameraController.h"
#include "IpcMessage.h"

// TensorStore utility modules
#include "TensorstoreJsonSpec.h"
#include "TensorstoreDataset.h"
#include "TensorstoreWriter.h"
#include "TensorstoreResizer.h"
#include "TensorstorePerformanceMonitor.h"
#include "TensorstoreErrorHandler.h"
#include "TensorstoreFlushManager.h"

// C++ Standard Library
#include <iostream>
#include <string>
#include <chrono>
#include <array>
#include <functional>

// TensorStore
#include <tensorstore/index_space/dim_expression.h>

// C Standard Library
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

namespace {
    namespace kvstore = tensorstore::kvstore;
    using ::tensorstore::ChunkLayout;
    using ::tensorstore::Context;
    using ::tensorstore::DimensionIndex;
    using ::tensorstore::DimensionSet;

    constexpr uint64_t kDatasetExpansionIncrement = 1000;  // Frames to add when expanding dataset
}

namespace FrameProcessor
{
    // Initializes all member variables, loads configuration,
    // and creates the output DPDK rings
    TensorstoreCore::TensorstoreCore(
        int fb_idx, int socket_id, DpdkWorkCoreReferences &dpdkWorkCoreReferences
    ) :
        DpdkWorkerCore(socket_id), 
        logger_(Logger::getLogger("FP.TensorstoreCore")),
        proc_idx_(fb_idx),
        decoder_(dpdkWorkCoreReferences.decoder),
        shared_buf_(dpdkWorkCoreReferences.shared_buf),
        last_frame_(0),
        processed_frames_(0),
        clear_frames_ring_(NULL),
        upstream_ring_(NULL),
        tensorstore_initialized_(false),
        data_type_("uint16"),
        pixel_type_(PixelDataType::UINT16),
        perf_monitor_(),
        frames_written_(0),
        write_errors_(0),
        avg_write_time_us_(0),
        pending_writes_count_(0),
        frames_forwarded_(0),
        completed_writes_(0),
        run_start_time_(0),
        first_write_time_(0),
        first_write_recorded_(false),
        frames_per_second_(0)
    {
        // Load configuration
        config_.resolve(dpdkWorkCoreReferences.core_config);
        config_.height_ = decoder_->get_frame_y_resolution();
        config_.width_  = decoder_->get_frame_x_resolution();
        config_.bit_depth_ = decoder_->get_frame_bit_depth();

        // Prevent writes until acquisition starts to avoid incomplete datasets
        config_.enable_writing_ = false;
        
        // Normalize path to avoid double slashes when appending proc_idx_
        // if (!config_.path_.empty()) {
        //     if (config_.path_.back() == '/') {
        //         config_.path_.pop_back();
        //     }
        //     config_.path_ += "/proc_" + std::to_string(proc_idx_);
        // }

        LOG4CXX_INFO(logger_, "FP.TensorstoreCore " << proc_idx_ << " Created with config:"
            << " core_name = " << config_.core_name
            << " connect = " << config_.connect
            << " upstream_core = " << config_.upstream_core
            << " num_cores = " << config_.num_cores
            << " num_downstream_cores = " << config_.num_downstream_cores
            << " storage_path = " << config_.storage_path_
            << " number_of_frames = " << config_.number_of_frames_
            << " frame_size = " << config_.frame_size_
            << " chunk_size = " << config_.chunk_size_
            << " cache_bytes_limit = " << config_.cache_bytes_limit_
            << " data_copy_concurrency = " << config_.data_copy_concurrency_
            << " delete_existing = " << config_.delete_existing_
            << " enable_writing = " << config_.enable_writing_
            << " path = " << config_.path_
            << " csv_logging = " << config_.csv_logging_
            << " csv_path = " << csv_path_
        );

        for (int ring_idx = 0; ring_idx < config_.num_downstream_cores; ring_idx++)
        {
            std::string downstream_ring_name = ring_name_str(config_.core_name, socket_id_, ring_idx);
            struct rte_ring* downstream_ring = rte_ring_lookup(downstream_ring_name.c_str());
            
            if (downstream_ring == NULL)
            {
                unsigned int downstream_ring_size = nearest_power_two(shared_buf_->get_num_buffers()*2);
                LOG4CXX_INFO(logger_, "Creating ring name "
                    << downstream_ring_name << " of size " << downstream_ring_size
                );
                
                downstream_ring = rte_ring_create(
                    downstream_ring_name.c_str(), downstream_ring_size, socket_id_, 0
                );
                
                if (downstream_ring == NULL)
                {
                    LOG4CXX_ERROR(logger_, "Error creating downstream ring " << downstream_ring_name
                        << " : " << rte_strerror(rte_errno)
                    );
                }
            }
            
            if (downstream_ring)
            {
                downstream_rings_.push_back(downstream_ring);
            }
        }
    }

    TensorstoreCore::~TensorstoreCore(void)
    {
        LOG4CXX_DEBUG_LEVEL(2, logger_, "TensorstoreCore destructor");
        csv_logger_.Close(logger_);
        stop();
    }

    // Runs the main loop for the worker core
    bool TensorstoreCore::run(unsigned int lcore_id)
    {
        lcore_id_ = lcore_id;
        run_lcore_ = true;
        run_start_time_ = rte_get_tsc_cycles();
        LOG4CXX_INFO(logger_, "TensorstoreCore: " << lcore_id_ << " starting up");
        

        ::SuperFrameHeader *current_frame_buffer;

        // Performance monitoring
        uint64_t start_frame_cycles = 0;
        uint64_t cycles_per_sec = rte_get_tsc_hz();

        while (likely(run_lcore_)) 
        {
            if (flush_pending_writes) {
                while (!pending_writes_queue_.empty()) {
                    pollAndProcessCompletions();
                }
                for (auto* frame_buf : frame_chunk_buffer_) {
                    uint64_t frame_num = decoder_->get_super_frame_number(frame_buf);
                    forwardFrame(frame_buf, frame_num);
                    frames_forwarded_++;
                }
                frame_chunk_buffer_.clear();

                auto flush_result = TensorstoreFlushManager::FlushPendingWrites(
                    store_,
                    tensorstore_initialized_,
                    highest_frame_written_,
                    current_dataset_capacity_,
                    decoder_->get_frame_y_resolution(),
                    decoder_->get_frame_x_resolution(),
                    logger_
                );
                
                frames_forwarded_ += flush_result.frames_forwarded;
                flush_pending_writes = false;
            }
            uint64_t now = rte_get_tsc_cycles();
            if (unlikely(perf_monitor_.ShouldUpdate(now, cycles_per_sec))) 
            {
                perf_monitor_.UpdateStatistics(cycles_per_sec);
            }

            pollAndProcessCompletions();

            if (tensorstore_initialized_ && store_.has_value() &&
                pending_writes_queue_.size() >= config_.max_concurrent_writes_)
            {
                perf_monitor_.RecordIdleLoop();
                continue;
            }

            if (rte_ring_dequeue(upstream_ring_, (void**) &current_frame_buffer) < 0)
            {
                perf_monitor_.RecordIdleLoop();
            }
            else
            {
                processed_frames_++;
                start_frame_cycles = rte_get_tsc_cycles();
                uint64_t frame_number = decoder_->get_super_frame_number(current_frame_buffer);
                LOG4CXX_DEBUG_LEVEL(2, logger_, "Dequeuing frame: " << frame_number);
                last_frame_ = frame_number;

                if (config_.enable_writing_ && config_.number_of_frames_ > 0 && frame_number >= config_.number_of_frames_)
                {
                    LOG4CXX_WARN(logger_, config_.core_name << " : " << proc_idx_ 
                        << " Processed all frames from acquisition. (" << config_.number_of_frames_ 
                        << "). Disabling writing. Frame " << frame_number 
                        << " and subsequent frames will be forwarded only.");
                    
                    config_.enable_writing_ = false;
                    
                    if (tensorstore_initialized_) {
                        LOG4CXX_INFO(logger_, "Flushing " << pending_writes_queue_.size() 
                            << " pending writes...");
                        
                        flush_pending_writes = true;
                    }
                }

                if (!config_.enable_writing_)
                {
                    forwardFrame(current_frame_buffer, frame_number);
                    frames_forwarded_++;
                    
                    uint64_t cycles_spent = rte_get_tsc_cycles() - start_frame_cycles;
                    perf_monitor_.RecordFrameProcessing(cycles_spent);
                    perf_monitor_.FramesThisSecond()++;
                    processed_frames_++;
                }
                else if (!tensorstore_initialized_)
                {
                    LOG4CXX_DEBUG_LEVEL(2, logger_, "Tensorstore not initialized. Forwarding frame " 
                        << frame_number << " without writing.");
                    forwardFrame(current_frame_buffer, frame_number);
                    frames_forwarded_++;
                    
                    uint64_t cycles_spent = rte_get_tsc_cycles() - start_frame_cycles;
                    perf_monitor_.RecordFrameProcessing(cycles_spent);
                    perf_monitor_.FramesThisSecond()++;
                }
                else if (tensorstore_initialized_ && store_.has_value() &&
                    pending_writes_queue_.size() < config_.max_concurrent_writes_)
                {
                    if (config_.number_of_frames_ > 0 && frame_number >= config_.number_of_frames_)
                    {
                        LOG4CXX_WARN(logger_, "Frame " << frame_number 
                            << " exceeds number_of_frames (" << config_.number_of_frames_ 
                            << "). Forwarding without writing.");
                        forwardFrame(current_frame_buffer, frame_number);
                        frames_forwarded_++;
                        
                        uint64_t cycles_spent = rte_get_tsc_cycles() - start_frame_cycles;
                        perf_monitor_.RecordFrameProcessing(cycles_spent);
                        perf_monitor_.FramesThisSecond()++;
                    }
                    else
                    {
                        if (TensorstoreResizer::NeedsExpansion(frame_number, current_dataset_capacity_)) {
                            const tensorstore::Index height = decoder_->get_frame_y_resolution();
                            const tensorstore::Index width = decoder_->get_frame_x_resolution();
                            
                            current_dataset_capacity_ = TensorstoreResizer::ExpandDataset(
                                *store_,
                                current_dataset_capacity_,
                                kDatasetExpansionIncrement,
                                height,
                                width,
                                logger_
                            );
                        }

                        if (frame_number > highest_frame_written_) {
                            highest_frame_written_ = frame_number;
                        }

                        void* frame_ptr = static_cast<void*>(current_frame_buffer);
                        const tensorstore::Index height = decoder_->get_frame_y_resolution();
                        const tensorstore::Index width = decoder_->get_frame_x_resolution();

                        tensorstore::WriteFutures write_future;
                        bool valid_pixel_type = true;

                        switch(pixel_type_) {
                            case PixelDataType::UINT8:
                                write_future = TensorstoreWriter::AsyncWriteFrame<uint8_t>(
                                    *store_, frame_ptr, height, width,
                                    frame_number, logger_
                                );
                                break;
                            case PixelDataType::UINT16:
                                write_future = TensorstoreWriter::AsyncWriteFrame<uint16_t>(
                                    *store_, frame_ptr, height, width,
                                    frame_number, logger_
                                );
                                break;
                            case PixelDataType::UINT32:
                                write_future = TensorstoreWriter::AsyncWriteFrame<uint32_t>(
                                    *store_, frame_ptr, height, width,
                                    frame_number, logger_
                                );
                                break;
                            case PixelDataType::UINT64:
                                write_future = TensorstoreWriter::AsyncWriteFrame<uint64_t>(
                                    *store_, frame_ptr, height, width,
                                    frame_number, logger_
                                );
                                break;
                            default:
                                LOG4CXX_ERROR(logger_, "Unexpected pixel type in async write for frame " 
                                    << frame_number << ". Forwarding without writing.");
                                valid_pixel_type = false;
                                break;
                        }
                        
                        // Forwards frame without writing if there is a pixel type error
                        if (!valid_pixel_type) {
                            forwardFrame(current_frame_buffer, frame_number);
                            frames_forwarded_++;
                            uint64_t cycles_spent = rte_get_tsc_cycles() - start_frame_cycles;
                            perf_monitor_.RecordFrameProcessing(cycles_spent);
                            perf_monitor_.FramesThisSecond()++;
                            continue;
                        }

                        if (!first_write_recorded_) {
                            first_write_time_ = start_frame_cycles;
                            first_write_recorded_ = true;
                        }

                        PendingWrite pw{
                            .frame_number = frame_number,
                            .frame_buffers = {current_frame_buffer},
                            .write_future = std::move(write_future),
                            .start_cycles = start_frame_cycles
                        };
                        pending_writes_queue_.insert({frame_number, std::move(pw)});

                        uint64_t cycles_spent = rte_get_tsc_cycles() - start_frame_cycles;
                        perf_monitor_.RecordFrameProcessing(cycles_spent);
                        perf_monitor_.FramesThisSecond()++;
                    }
                }
                else
                {
                    LOG4CXX_ERROR(logger_, "Unexpected state for frame " << frame_number << ". Forwarding.");
                    forwardFrame(current_frame_buffer, frame_number);
                    frames_forwarded_++;
                    
                    uint64_t cycles_spent = rte_get_tsc_cycles() - start_frame_cycles;
                    perf_monitor_.RecordFrameProcessing(cycles_spent);
                    perf_monitor_.FramesThisSecond()++;
                }
                
            }
        }

        LOG4CXX_INFO(logger_, "Core " << lcore_id_ << " completed");
        return true;
    }

    // Polls the unordered_map (pending_writes_queue_)and processes any completed writes
    void TensorstoreCore::pollAndProcessCompletions()
    {
        for (auto it = pending_writes_queue_.begin(); it != pending_writes_queue_.end(); )
        {
            PendingWrite& pending = it->second;
            if (pending.write_future.commit_future.ready())
            {
                uint64_t end_cycles = rte_get_tsc_cycles();
                uint64_t cycles_elapsed = end_cycles - pending.start_cycles;
                uint64_t write_time_us = (cycles_elapsed * 1000000) / rte_get_tsc_hz();
                const auto& result = pending.write_future.commit_future.result();
                if (!result.ok()) {
                    ++write_errors_;
                    LOG4CXX_ERROR(logger_, "Error writing frame chunk starting at " << pending.frame_number 
                        << " to " << config_.storage_driver_ << ": " << result.status());
                    csv_logger_.LogWrite(
                        pending.frame_number,
                        pending.frame_buffers.size(),
                        write_time_us,
                        false,
                        frames_written_,
                        avg_write_time_us_,
                        lcore_id_,
                        frames_per_second_,
                        first_write_time_,
                        rte_get_tsc_hz()
                    );
                } else {
                    frames_written_ += pending.frame_buffers.size();
                    ++completed_writes_;
                    // Running average is calculated incrementally to avoid overflow
                    uint64_t total_write_time_us = avg_write_time_us_ * (completed_writes_ - 1);
                    avg_write_time_us_ = (total_write_time_us + write_time_us) / completed_writes_;
                    LOG4CXX_DEBUG_LEVEL (2, logger_, "Successfully wrote " << pending.frame_buffers.size()
                        << " frames starting at " << pending.frame_number
                        << " to " << config_.storage_driver_ << " in " << write_time_us << " us");
                    csv_logger_.LogWrite(
                        pending.frame_number,
                        pending.frame_buffers.size(),
                        write_time_us,
                        true,
                        frames_written_,
                        avg_write_time_us_,
                        lcore_id_,
                        frames_per_second_,
                        first_write_time_,
                        rte_get_tsc_hz()
                    );
                }
                for (auto* frame_buf : pending.frame_buffers) {
                    uint64_t frame_num = decoder_->get_super_frame_number(frame_buf);
                    forwardFrame(frame_buf, frame_num);
                    frames_forwarded_++;
                }
                it = pending_writes_queue_.erase(it);
            }
            else
            {
                ++it;
            }
        }
        pending_writes_count_ = pending_writes_queue_.size();
    }

    // Handles the closing of the old store and creation of a new one.
    void TensorstoreCore::handleReconfiguration()
    {
        if (tensorstore_initialized_) 
        {
            LOG4CXX_INFO(logger_, "Reconfiguration requested. Flushing " 
                << pending_writes_queue_.size() << " pending writes...");
            
            // Wait for all pending writes to complete and forward frames
            while (!pending_writes_queue_.empty()) {
                pollAndProcessCompletions();
            }
            LOG4CXX_INFO(logger_, "All pending writes completed");
            
            // Forward any buffered frames
            for (auto* frame_buf : frame_chunk_buffer_) {
                uint64_t frame_num = decoder_->get_super_frame_number(frame_buf);
                forwardFrame(frame_buf, frame_num);
                frames_forwarded_++;
            }
            frame_chunk_buffer_.clear();
            
            // Close the existing store
            if (store_.has_value()) {
                store_.reset();
            }
            tensorstore_initialized_ = false;
        }
        

        pending_writes_queue_.clear();
        frame_chunk_buffer_.clear();
        
        // Reset all counters for a new acquisition
        processed_frames_ = 0;
        frames_forwarded_ = 0;
        frames_written_ = 0;
        completed_writes_ = 0;
        pending_writes_count_ = 0;
        write_errors_ = 0;
        avg_write_time_us_ = 0;
        last_frame_ = 0;
        highest_frame_written_ = 0;
        current_dataset_capacity_ = 0;
        first_write_recorded_ = false;
        last_error_message_ = "";
        
        // Re-enable writing for the new dataset
        config_.enable_writing_ = true;
        
        // Query camera for frame rate to ensure CSV timestamps are accurate.
        CameraController* camera_controller = CameraController::Instance("CameraController_");
        if (camera_controller) {
            OdinData::IpcMessage temp_reply;
            if (camera_controller->request_configuration("", temp_reply)) {
                if (temp_reply.has_param("camera/frames_per_second")) {
                    frames_per_second_ = temp_reply.get_param<unsigned int>("camera/frames_per_second");
                    LOG4CXX_INFO(logger_, "Updated frames_per_second from camera config: " << frames_per_second_);
                }
            }
        }
        
        // Creates a new CSV file for the new acquisition to avoid data mixing
        if (config_.csv_logging_ && !config_.csv_path_.empty()) {
            csv_logger_.Close(logger_);
            
            std::string csv_filename = config_.csv_path_;
            
            // Get current timestamp
            time_t now = time(nullptr);
            struct tm* timeinfo = localtime(&now);
            char timestamp[32];
            strftime(timestamp, sizeof(timestamp), "%Y%m%d_%H%M%S", timeinfo);
       
            size_t dot_pos = csv_filename.find_last_of('.');
            if (dot_pos != std::string::npos) {
                csv_filename.insert(dot_pos, "_" + std::string(timestamp) + "_" + config_.kvstore_driver_);
            } else {
                csv_filename += "_" + std::string(timestamp) + "_" + config_.kvstore_driver_ + ".csv";
            }
            csv_path_ = csv_filename;
            
            csv_logger_.Open(csv_path_, logger_);
            LOG4CXX_INFO(logger_, "New CSV log file created: " << csv_path_);
        }

        // Create the new dataset
        std::size_t height = config_.height_;
        std::size_t width  = config_.width_;
        std::size_t bit_depth = config_.bit_depth_;

        LOG4CXX_INFO(logger_, "Creating new dataset with:"
            << " path=" << config_.path_
            << " width=" << width
            << " height=" << height
            << " bit_depth=" << bit_depth
            << " number_of_frames=" << config_.number_of_frames_
        );

        // Map byte-size bit depth to TensorStore data type.
        switch(bit_depth) {
            case 1:
                data_type_ = "uint8";
                pixel_type_ = PixelDataType::UINT8;
                break;
            case 2:
                data_type_ = "uint16";
                pixel_type_ = PixelDataType::UINT16;
                break;
            case 4:
                data_type_ = "uint32";
                pixel_type_ = PixelDataType::UINT32;
                break;
            case 8:
                data_type_ = "uint64";
                pixel_type_ = PixelDataType::UINT64;
                break;
            default:
                LOG4CXX_ERROR(logger_, "Unsupported bit depth: " << bit_depth);
                last_error_message_ = "Unsupported bit depth: " + std::to_string(bit_depth);
                return;
        }
        // Create JSON spec for the new dataset
        // Uses 1000 as default capacity when frame count is unknown.
        std::size_t initial_frames = (config_.number_of_frames_ > 0) ? config_.number_of_frames_ : 1000;
        ::nlohmann::json json_spec = FrameProcessor::GetJsonSpec(
            config_.storage_driver_, config_.kvstore_driver_, 
            config_.s3_bucket_, config_.s3_endpoint_,
            data_type_, config_.path_, 
            initial_frames, height, width, config_.cache_bytes_limit_
        );
        
        // Create the store
        auto store_result = CreateDataset(json_spec);
        
        if (!store_result.ok()) {
            std::string error_msg = store_result.status().ToString();
            last_error_message_ = TensorstoreErrorHandler::FormatDatasetCreationError(
                error_msg,
                config_.kvstore_driver_,
                config_.s3_endpoint_,
                config_.path_
            );
            LOG4CXX_ERROR(logger_, last_error_message_);
        } else {
            store_ = std::move(store_result.value());
            tensorstore_initialized_ = true;
            current_dataset_capacity_ = initial_frames;
            highest_frame_written_ = 0;
            frames_written_ = 0;
            write_errors_ = 0;
            last_error_message_ = "";
            first_write_recorded_ = false;
            LOG4CXX_INFO(logger_, "Dataset created/opened successfully with initial capacity of " 
                << initial_frames << " frames. Data will be written to: " << config_.path_);
        }
    }

    // Forwards a frame buffer to the correct downstream ring.
    void TensorstoreCore::forwardFrame(::SuperFrameHeader* frame_buffer, uint64_t frame_number)
    {
        int ret;
        
        // Returns frame directly to clear_frames_ring_ if there are no downstream cores
        if (config_.num_downstream_cores == 0 || downstream_rings_.empty()) {
            if (clear_frames_ring_ != NULL) {
                ret = rte_ring_enqueue(clear_frames_ring_, frame_buffer);
                if (ret != 0) {
                    LOG4CXX_ERROR(logger_, "Failed to return frame " << frame_number 
                        << " to clear_frames_ring: " << rte_strerror(-ret));
                } else {
                    LOG4CXX_DEBUG_LEVEL(3, logger_, "Returned frame " << frame_number 
                        << " to clear_frames_ring");
                }
            } else {
                LOG4CXX_ERROR(logger_, "clear_frames_ring is NULL, cannot return frame " << frame_number);
            }
            return;
        }
        
        // Distribute frames across downstream cores for load balancing 
        ret = rte_ring_enqueue(
            downstream_rings_[frame_number % (config_.num_downstream_cores)], 
            frame_buffer
        );
        if (ret != 0) {
            LOG4CXX_ERROR(logger_, "Failed to forward frame " << frame_number 
                << " to downstream ring: " << rte_strerror(-ret));
            
            // Attempt to return frame to clear_frames_ring_ if forwarding fails
            if (clear_frames_ring_ != NULL) {
                int clear_ret = rte_ring_enqueue(clear_frames_ring_, frame_buffer);
                if (clear_ret != 0) {
                    LOG4CXX_ERROR(logger_, "Failed to return frame " << frame_number 
                        << " to clear_frames_ring: " << rte_strerror(-clear_ret));
                } else {
                    LOG4CXX_DEBUG_LEVEL(2, logger_, "Returned frame " << frame_number 
                        << " to clear_frames_ring");
                }
            } else {
                LOG4CXX_ERROR(logger_, "Failed to return frame " << frame_number << ": clear_frames_ring is null");
            }
        }
    }

    // Stops the Tensorstore core's main loop.
    void TensorstoreCore::stop(void)
    {

        if (run_lcore_){
    
        if (tensorstore_initialized_) 
        {
            LOG4CXX_INFO(logger_, "Waiting for " 
                << pending_writes_queue_.size() << " pending writes...");
            
            while (!pending_writes_queue_.empty()) {
                pollAndProcessCompletions();
            }

            LOG4CXX_INFO(logger_, "Write queue has cleared");

            store_.reset();
            tensorstore_initialized_ = false;
        }
            LOG4CXX_INFO(logger_, "Stopping TensorstoreCore on lcore " << lcore_id_);
            run_lcore_ = false;
        }
    }

    // Reports the status of the Tensorstore core.
    void TensorstoreCore::status(OdinData::IpcMessage& status, const std::string& path)
    {
        std::string status_path = path + "/TensorstoreCore_" + std::to_string(proc_idx_) + "/";
        std::string ring_status = status_path + "upstream_rings/";
        std::string timing_status = status_path + "timing/";
        std::string ts_status = status_path + "tensorstore/";

        status.set_param(status_path + "frames_dequeued", processed_frames_);
        status.set_param(status_path + "frames_forwarded", frames_forwarded_);
        status.set_param(status_path + "frames_processed_per_second", perf_monitor_.GetFramesPerSecond());
        status.set_param(status_path + "idle_loops", perf_monitor_.GetIdleLoops());
        status.set_param(status_path + "core_usage", (int)perf_monitor_.GetCoreUsage());
        status.set_param(status_path + "last_frame_number_dequeued", last_frame_);

        status.set_param(timing_status + "mean_frame_us", perf_monitor_.GetMeanFrameTimeUs());
        status.set_param(timing_status + "max_frame_us", perf_monitor_.GetMaxFrameTimeUs());

        status.set_param(ring_status + ring_name_str(config_.upstream_core, socket_id_, proc_idx_) + "_count", rte_ring_count(upstream_ring_));
        status.set_param(ring_status + ring_name_str(config_.upstream_core, socket_id_, proc_idx_) + "_size", rte_ring_get_size(upstream_ring_));
        status.set_param(ring_status + ring_name_clear_frames(socket_id_) + "_count" , rte_ring_count(clear_frames_ring_));
        status.set_param(ring_status + ring_name_clear_frames(socket_id_) + "_size" , rte_ring_get_size(clear_frames_ring_));

        status.set_param(ts_status + "initialized", tensorstore_initialized_);
        status.set_param(ts_status + "storage_path", config_.path_);
        status.set_param(ts_status + "frames_written", frames_written_);
        status.set_param(ts_status + "write_errors", write_errors_);
        status.set_param(ts_status + "avg_write_time_us", avg_write_time_us_);
        status.set_param(ts_status + "pending_writes_queue_size", pending_writes_count_);
        status.set_param(ts_status + "total_completed_writes", completed_writes_);
        status.set_param(ts_status + "enable_writing", config_.enable_writing_);
        status.set_param(ts_status + "last_error", last_error_message_);
    }

    // Connects to input (upstream) rings.
    bool TensorstoreCore::connect(void)
    {
        std::string upstream_ring_name = ring_name_str(config_.upstream_core, socket_id_, proc_idx_);
        struct rte_ring* upstream_ring = rte_ring_lookup(upstream_ring_name.c_str());
        
        if (upstream_ring == NULL)
        {
            LOG4CXX_INFO(logger_, config_.core_name << " : " << proc_idx_ << " Failed to Connect to upstream resources! (" << upstream_ring_name << ")");
            return false;
        }
        else
        {
            upstream_ring_ = upstream_ring; 
        }

        std::string clear_frames_ring_name = ring_name_clear_frames(socket_id_);
        clear_frames_ring_ = rte_ring_lookup(clear_frames_ring_name.c_str());
        
        if (clear_frames_ring_ == NULL)
        {
            LOG4CXX_INFO(logger_, config_.core_name << " : " << proc_idx_ << " Failed to Connect to upstream resources!");
            return false;
        }
        
        LOG4CXX_INFO(logger_, config_.core_name << " : " << proc_idx_ << " Connected to upstream resources successfully!");
        return true;
    }

    // Configures the Tensorstore core.
    void TensorstoreCore::configure(OdinData::IpcMessage& config)
    {
        LOG4CXX_INFO(logger_, config_.core_name << " : " << proc_idx_ << " Got update config.");

        try {
        if (config.has_param("path"))
        {
            config_.path_ = config.get_param<std::string>("path");
            LOG4CXX_INFO(logger_, config_.core_name << " : " << proc_idx_ << " Setting path_ to: " <<  config_.path_);
        }

        if (config.has_param("storage_driver")) {
            config_.storage_driver_ = config.get_param<std::string>("storage_driver");
            LOG4CXX_INFO(logger_, config_.core_name << " : " << proc_idx_ << " Setting storage_driver_ to: " <<  config_.storage_driver_);
        }

        if (config.has_param("kvstore_driver")) {
            config_.kvstore_driver_ = config.get_param<std::string>("kvstore_driver");
            LOG4CXX_INFO(logger_, config_.core_name << " : " << proc_idx_ << " Setting kvstore_driver_ to: " <<  config_.kvstore_driver_);
        }

        if (config.has_param("max_concurrent_writes")) {
            config_.max_concurrent_writes_ = config.get_param<int>("max_concurrent_writes");
            LOG4CXX_INFO(logger_, config_.core_name << " : " << proc_idx_ << " Setting max_concurrent_writes to: " <<  config_.max_concurrent_writes_);
        }

        if (config.has_param("number_of_frames")) {
            config_.number_of_frames_ = config.get_param<uint64_t>("number_of_frames");
            LOG4CXX_INFO(logger_, config_.core_name << " : " << proc_idx_ << " Setting number_of_frames to: " <<  config_.number_of_frames_);
        }

        if (config.has_param("frames_per_second")) {
            frames_per_second_ = config.get_param<unsigned int>("frames_per_second");
            LOG4CXX_INFO(logger_, config_.core_name << " : " << proc_idx_ << " Setting frames_per_second to: " << frames_per_second_);
        }

        if (config.has_param("enable_writing")) {
            bool previous_state = config_.enable_writing_;
            config_.enable_writing_ = config.get_param<bool>("enable_writing");
            
            if (config_.enable_writing_ != previous_state) {
                LOG4CXX_INFO(logger_, config_.core_name << " : " << proc_idx_ 
                    << " Setting enable_writing to: " << config_.enable_writing_);
                
                if (!config_.enable_writing_ && tensorstore_initialized_) {
                    LOG4CXX_DEBUG_LEVEL(2, logger_, "Writing disabled. Flushing " 
                        << pending_writes_queue_.size() << " pending writes");
                    
                    flush_pending_writes = true;
                }
            }
        }

        if (config.has_param("update_config")) {
            LOG4CXX_INFO(logger_, config_.core_name << " : " << proc_idx_ << " update_config is " << config.get_param<bool>("update_config") << ".");
       
        if (config.get_param<bool>("update_config") == true) {
            handleReconfiguration();
        } else {
            LOG4CXX_INFO(logger_, config_.core_name << " : " << proc_idx_ << " No config changes detected.");
        } 
        }

        }catch (const std::exception& e) {
                    LOG4CXX_ERROR(logger_, "Failed to get configuration: " << e.what());
        }


    }

    DPDKREGISTER(DpdkWorkerCore, TensorstoreCore, "TensorstoreCore");

} // End of the FrameProcessor namespace