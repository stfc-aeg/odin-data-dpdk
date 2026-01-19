////////////////////////////////////////////////////////////////////////////////
// SECTION 1: REQUIRED CODE FILES
////////////////////////////////////////////////////////////////////////////////

#include "TensorstoreCore.h"

// DPDK Includes
#include "DpdkUtils.h"
#include "DpdkSharedBufferFrame.h"

// C++ Standard Library Includes
#include <iostream>
#include <string>
#include <chrono>
#include <deque>

// TensorStore Includes
#include <tensorstore/tensorstore.h>
#include <tensorstore/util/status.h>
#include <tensorstore/context.h>
#include <tensorstore/open.h>
#include <tensorstore/open_mode.h>
#include <tensorstore/index.h>
#include <tensorstore/index_space/dim_expression.h>

// C Standard Library Includes
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

// Additional C++ Utility Includes
#include <array>
#include <atomic>
#include <functional>
#include <typeinfo>
#include <optional>

////////////////////////////////////////////////////////////////////////////////
// SECTION 2: CODE ORGANIZATION AND NAMING
////////////////////////////////////////////////////////////////////////////////

namespace {
    namespace kvstore = tensorstore::kvstore;
    using ::tensorstore::ChunkLayout;
    using ::tensorstore::Context;
    using ::tensorstore::DimensionIndex;
    using ::tensorstore::DimensionSet;
}


////////////////////////////////////////////////////////////////////////////////
// SECTION 3: STORAGE CONFIGURATION FUNCTION
////////////////////////////////////////////////////////////////////////////////

/**
 * Function: GetJsonSpec
 * Purpose: Creates a "JSON" object that defines the complete storage layout
 * for TensorStore (driver, path, data type, shape, chunking).
 */
::nlohmann::json GetJsonSpec(const std::string& storage_driver, const std::string& kvstore_driver, const std::string& s3_bucket, const std::string& s3_endpoint, const std::string& data_type, const std::string& path, std::size_t frames, std::size_t height, std::size_t width, uint64_t cache_bytes_limit, std::size_t frames_per_chunk) {
    
    // Build kvstore specification based on driver type
    ::nlohmann::json kvstore_spec;
    
    if (kvstore_driver == "s3") {
        kvstore_spec = {
            {"driver", "s3"},
            {"bucket", s3_bucket},
            {"endpoint", s3_endpoint},
            {"path", path}
        };
    } else {
        kvstore_spec = {
            {"driver", "file"},
            {"path", path}
        };
    }
    
    ::nlohmann::json json_spec = {
        {"driver", storage_driver},
        {"kvstore", kvstore_spec},
        {"metadata", {
            {"data_type", data_type},
            {"shape", {frames, height, width}},
            {"chunk_grid", {
                {"name", "regular"},
                {"configuration", {
                    {"chunk_shape", {frames_per_chunk, height, width}}
                }}
            }}
        }}
    };
    
    if (kvstore_driver == "file") {
        json_spec["context"] = {
            {"data_copy_concurrency", {
                {"limit", "shared"}
            }},
            {"cache_pool", {
                {"total_bytes_limit", cache_bytes_limit}
            }}
        };
    }
    
    return json_spec;
}


// ::nlohmann::json GetJsonSpec()
// {
//   return {
//     {"driver", "zarr3"},
//     {"kvstore", {
//       {"driver", "s3"}, 
//       {"bucket",  "tensorstore-objects"},
//       {"endpoint", "https://s3.echo.stfc.ac.uk"},
//       {"path", "odin-data-db"}
//     }
//     },
//     {"metadata",
//       {
//         {"data_type", "uint16"},
//         {"shape", {2304,4096}},
//         {"chunk_grid", {
//           {"name", "regular"},
//           {"configuration", {
//             {"chunk_shape", {2304,4096}}
//           }}
//         }}
//       }}
//     };
//   }

////////////////////////////////////////////////////////////////////////////////
// SECTION 4: STORAGE CREATION AND ASYNC WRITE FUNCTIONS
////////////////////////////////////////////////////////////////////////////////

/**
 * Function: create_dataset
 * Purpose: Takes the JSON settings and creates/opens the TensorStore.
 * This is a blocking call meaning it is intended to only be run once at initialization.
 */
static tensorstore::Result<tensorstore::TensorStore<>> create_dataset(const ::nlohmann::json& json_spec) {
    auto context = Context::Default();
    auto store_result = tensorstore::Open(
        json_spec,
        context,
        tensorstore::OpenMode::create | tensorstore::OpenMode::open,
        tensorstore::ReadWriteMode::read_write
    ).result(); 
    
    return store_result;
}

/**
 * Function: TensorstoreCore::asyncWriteFrame (Template Function)
 * Purpose: This is a non-blocking function that initiates a write.
 *
 * It handles several tasks:
 * 1. Changes the raw data pointer to the correct C++ type (T).
 * 2. Creates a "view" of the 2D image data without copying it.
 * 3. Checks if the TensorStore needs to be resized.
 * 4. Selects the correct 2D "slice" in the 3D store.
 * 5. Calls `tensorstore::Write` which returns a `Future<void>`.
 * 6. Returns this `Future` immediately.
 */
namespace FrameProcessor {

template <typename T>
tensorstore::WriteFutures TensorstoreCore::asyncWriteFrame(
    tensorstore::TensorStore<>& store,
    void* raw_data,
    tensorstore::Index height,
    tensorstore::Index width,
    uint64_t frame_number) 
{
    // 1. Cast raw memory to the correct data type.
    T* data_as_T = reinterpret_cast<T*>(raw_data);

    // 2. Describes the 2D image's memory layout (shape and strides)
    // Shape is [height, width] for row-major storage
    std::array<tensorstore::Index, 2> shape = {height, width};
    std::array<tensorstore::Index, 2> byte_strides = {
        width * static_cast<tensorstore::Index>(sizeof(T)),  // stride to next row
        static_cast<tensorstore::Index>(sizeof(T))            // stride to next column
    };
    tensorstore::StridedLayout<2> layout(shape, byte_strides);

    // 3. Create a "view" of the data (zero-copy)
    tensorstore::ArrayView<T, 2> two_dim_view(data_as_T, layout);

    // 4. Convert the view to a "shared" array for safe writing
    auto array2 = tensorstore::StaticRankCast<2>(tensorstore::UnownedToShared(two_dim_view));

    // 5. Check if the store needs to be resized.
    auto domain = store.domain();
    auto bounds = domain.box();
    tensorstore::Index current_num_frames = bounds.shape()[0];
    tensorstore::Index target_frame_index = static_cast<tensorstore::Index>(frame_number);

    if (target_frame_index >= current_num_frames) {
        tensorstore::Index new_size = target_frame_index + 1;
        std::array<tensorstore::Index, 3> inclusive_min = {0, 0, 0};
        std::array<tensorstore::Index, 3> exclusive_max = {new_size, height, width};
        
        tensorstore::span<const tensorstore::Index> inclusive_min_span(inclusive_min.data(), inclusive_min.size());
        tensorstore::span<const tensorstore::Index> exclusive_max_span(exclusive_max.data(), exclusive_max.size());
        
        // Execute the resize operation (BLOCKING)
        auto resize_status = tensorstore::Resize(
            store,
            inclusive_min_span,
            exclusive_max_span,
            tensorstore::ResizeMode::expand_only
        ).result();
        
        if (!resize_status.ok()) {
            LOG4CXX_ERROR(logger_, "Failed to resize dataset for frame " << frame_number 
                << ": " << resize_status.status());
        }
    }

    // 6. Select the target 2D slice
    std::array<tensorstore::Index, 1> indices = {target_frame_index};
    auto target = store | tensorstore::Dims(0).IndexSlice(indices);

    // 7. Write the 2D image data `array2` into the `target` slice
    LOG4CXX_DEBUG_LEVEL(2, logger_, "Calling write for frame " << frame_number);
    auto write_future = tensorstore::Write(array2, target);

    // 8. Return the future immediately.
    return write_future;
}

/**
 * Function: TensorstoreCore::asyncWriteFrameChunk (Template Function)
 * Purpose: This is a non-blocking function that initiates a chunked write.
 *
 * It handles several tasks:
 * 1. Allocates contiguous memory and copies multiple frames into a 3D array.
 * 2. Checks if the TensorStore needs to be resized.
 * 3. Selects the correct 3D "slice" in the 3D store.
 * 4. Calls `tensorstore::Write` which returns a `Future<void>`.
 * 5. Returns this `Future` immediately.
 */
template <typename T>
tensorstore::WriteFutures TensorstoreCore::asyncWriteFrameChunk(
    tensorstore::TensorStore<>& store,
    const std::vector<void*>& raw_data_ptrs,
    tensorstore::Index height,
    tensorstore::Index width,
    uint64_t start_frame_number,
    size_t num_frames)
{
    // 1. Allocate contiguous memory and copy multiple frames into a 3D array
    std::array<tensorstore::Index, 3> shape = {
        static_cast<tensorstore::Index>(num_frames), 
        height, 
        width
    };
    
    size_t total_elements = num_frames * height * width;
    
    auto shared_array = tensorstore::AllocateArray<T>(shape);
    
    size_t frame_elements = height * width;
    for (size_t i = 0; i < num_frames; ++i) {
        T* src = reinterpret_cast<T*>(raw_data_ptrs[i]);
        T* dst = shared_array.data() + (i * frame_elements);
        std::copy(src, src + frame_elements, dst);
    }
    
    // 2. Check if the store needs to be resized
    auto domain = store.domain();
    auto bounds = domain.box();
    tensorstore::Index current_num_frames = bounds.shape()[0];
    tensorstore::Index target_frame_index = static_cast<tensorstore::Index>(start_frame_number + num_frames - 1);

    if (target_frame_index >= current_num_frames) {
        tensorstore::Index new_size = target_frame_index + 1;
        std::array<tensorstore::Index, 3> inclusive_min = {0, 0, 0};
        std::array<tensorstore::Index, 3> exclusive_max = {new_size, height, width};
        
        tensorstore::span<const tensorstore::Index> inclusive_min_span(inclusive_min.data(), inclusive_min.size());
        tensorstore::span<const tensorstore::Index> exclusive_max_span(exclusive_max.data(), exclusive_max.size());
        
        // Execute the resize operation (BLOCKING)
        auto resize_status = tensorstore::Resize(
            store,
            inclusive_min_span,
            exclusive_max_span,
            tensorstore::ResizeMode::expand_only
        ).result();
        
        if (!resize_status.ok()) {
            LOG4CXX_ERROR(logger_, "Failed to resize dataset for frame chunk starting at " 
                << start_frame_number << ": " << resize_status.status());
        }
    }

    // 3. Select the target 3D slice
    std::array<tensorstore::Index, 1> start_indices = {static_cast<tensorstore::Index>(start_frame_number)};
    std::array<tensorstore::Index, 1> end_indices = {static_cast<tensorstore::Index>(start_frame_number + num_frames)};
    
    auto target = store 
        | tensorstore::Dims(0).HalfOpenInterval(start_indices[0], end_indices[0]);
    
    LOG4CXX_DEBUG_LEVEL(2, logger_, "Writing chunk of " << num_frames 
        << " frames starting at " << start_frame_number);
    
    // 4. Write the 3D array data into the `target` slice
    auto write_future = tensorstore::Write(shared_array, target);
    
    // 5. Return the future immediately
    return write_future;
}
}
namespace FrameProcessor
{
    /**
     * Function: TensorstoreCore::TensorstoreCore (Constructor)
     * Purpose: Initializes all member variables, loads configuration,
     * and creates the *output* DPDK rings.
     */
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
        processed_frames_hz_(0),
        idle_loops_(0),
        mean_us_on_frame_(0),
        maximum_us_on_frame_(0),
        core_usage_(0),
        clear_frames_ring_(NULL),
        upstream_ring_(NULL),
        tensorstore_initialized_(false),
        data_type_("uint16"),
        pixel_type_(PixelDataType::UINT16),
        total_frames_(0),
        frame_size_(0),
        frames_written_(0),
        write_errors_(0),
        avg_write_time_us_(0),
        pending_writes_count_(0),
        frames_forwarded_(0),
        completed_writes_(0),
        csv_logging_enabled_(false),
        run_start_time_(0),
        first_write_time_(0),
        first_write_recorded_(false)
    {
        // Load configuration
        config_.resolve(dpdkWorkCoreReferences.core_config);
        config_.height_ = decoder_->get_frame_y_resolution();
        config_.width_  = decoder_->get_frame_x_resolution();
        config_.bit_depth_ = decoder_->get_frame_bit_depth();
        
        // Appends proc_idx_ to the dataset path so each core is writing into different locations
        if (!config_.path_.empty()) {
            if (config_.path_.back() == '/') {
                config_.path_.pop_back();
            }
            // config_.path_ += "/" + std::to_string(proc_idx_);
        }
        
    
        if (config_.csv_logging_ && !config_.csv_path_.empty()) {
            std::string csv_filename = config_.csv_path_;
            
            // Get current timestamp
            time_t now = time(nullptr);
            struct tm* timeinfo = localtime(&now);
            char timestamp[32];
            strftime(timestamp, sizeof(timestamp), "%Y%m%d_%H%M%S", timeinfo);
       
            size_t dot_pos = csv_filename.find_last_of('.');
            if (dot_pos != std::string::npos) {
                csv_filename.insert(dot_pos, "_" + std::string(timestamp) + "_core" + std::to_string(proc_idx_));
            } else {
                csv_filename += "_" + std::string(timestamp) + "_core" + std::to_string(proc_idx_) + ".csv";
            }
            csv_path_ = csv_filename;
        }

        LOG4CXX_INFO(logger_, "FP.TensorstoreCore " << proc_idx_ << " Created with config:"
            << " core_name = " << config_.core_name
            << " connect = " << config_.connect
            << " upstream_core = " << config_.upstream_core
            << " num_cores = " << config_.num_cores
            << " num_downstream_cores = " << config_.num_downstream_cores
            << " storage_path = " << config_.storage_path_
            << " max_frames = " << config_.max_frames_
            << " frame_size = " << config_.frame_size_
            << " chunk_size = " << config_.chunk_size_
            << " cache_bytes_limit = " << config_.cache_bytes_limit_
            << " data_copy_concurrency = " << config_.data_copy_concurrency_
            << " delete_existing = " << config_.delete_existing_
            << " frames_per_chunk = " << config_.frames_per_chunk_
            << " enable_writing = " << config_.enable_writing_
            << " path = " << config_.path_
            << " csv_logging = " << config_.csv_logging_
            << " csv_path = " << csv_path_
        );

        // --- DPDK Output Ring Initialization ---
        for (int ring_idx = 0; ring_idx < config_.num_downstream_cores; ring_idx++)
        {
            std::string downstream_ring_name = ring_name_str(config_.core_name, socket_id_, ring_idx);
            struct rte_ring* downstream_ring = rte_ring_lookup(downstream_ring_name.c_str());
            
            if (downstream_ring == NULL)
            {
                unsigned int downstream_ring_size = nearest_power_two(shared_buf_->get_num_buffers());
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

    /**
     * Function: TensorstoreCore::~TensorstoreCore (Destructor)
     */
    TensorstoreCore::~TensorstoreCore(void)
    {
        LOG4CXX_DEBUG_LEVEL(2, logger_, "TensorstoreCore destructor");
        closeCSVLog();
        stop();
    }

    /**
     * Function: TensorstoreCore::run
     * Purpose: The main loop for the worker core
     */
    bool TensorstoreCore::run(unsigned int lcore_id)
    {
        lcore_id_ = lcore_id;
        run_lcore_ = true;
        run_start_time_ = rte_get_tsc_cycles();
        LOG4CXX_INFO(logger_, "TensorstoreCore: " << lcore_id_ << " starting up");
        
        // Opens the CSV log if enabled in the config
        if (config_.csv_logging_ && !csv_path_.empty()) {
            openCSVLog(csv_path_);
        }

        struct SuperFrameHeader *current_frame_buffer;

        // Performance monitoring variables
        uint64_t frames_per_second = 1;
        uint64_t total_frame_cycles = 1;
        uint64_t cycles_working = 1;
        uint64_t maximum_frame_cycles = 0;
        uint64_t idle_loops = 0;
        uint64_t start_frame_cycles = 0;
        uint64_t last = rte_get_tsc_cycles();
        uint64_t cycles_per_sec = rte_get_tsc_hz();

        handleReconfiguration();

        // --- Main Worker Loop ---
        while (likely(run_lcore_)) 
        {
            // --- Performance Stats Update (runs approx once per second) ---
            uint64_t now = rte_get_tsc_cycles();
            if (unlikely((now - last) >= (cycles_per_sec))) //Unlikely means this condition is rarely true
            {
                processed_frames_hz_ = frames_per_second - 1;
                mean_us_on_frame_ = (total_frame_cycles * 1000000) / (frames_per_second * cycles_per_sec);
                core_usage_ = (cycles_working * 255) / cycles_per_sec;
                maximum_us_on_frame_ = (maximum_frame_cycles * 1000000) / (cycles_per_sec);
                idle_loops_ = idle_loops;

                // Reset counters
                frames_per_second = 1;
                idle_loops = 0;
                total_frame_cycles = 1;
                cycles_working = 1;
                last = now;
            }
            

            // This function checks the front of the pending_writes_queue_
            // and processes any completed writes.
            pollAndProcessCompletions();

            // Dequeue frames and handle based on writing mode
            if (rte_ring_dequeue(upstream_ring_, (void**) &current_frame_buffer) < 0)
            {
                // If the ring is empty, count it as an "idle" loop and try again
                idle_loops++;
            }
            else // Frame has been dequeued successfully
            {
                start_frame_cycles = rte_get_tsc_cycles();
                uint64_t frame_number = decoder_->get_super_frame_number(current_frame_buffer);
                LOG4CXX_DEBUG_LEVEL(2, logger_, "Dequeuing frame: " << frame_number);
                last_frame_ = frame_number;

                // Check if we've reached the maximum frames limit
                if (config_.enable_writing_ && config_.max_frames_ > 0 && frame_number >= config_.max_frames_)
                {
                    LOG4CXX_WARN(logger_, config_.core_name << " : " << proc_idx_ 
                        << " Reached maximum frames limit (" << config_.max_frames_ 
                        << "). Disabling writing. Frame " << frame_number 
                        << " and subsequent frames will be forwarded only.");
                    
                    config_.enable_writing_ = false;
                    
                    // Flush pending writes
                    if (tensorstore_initialized_) {
                        LOG4CXX_INFO(logger_, "Flushing " << pending_writes_queue_.size() 
                            << " pending writes...");
                        
                        while (!pending_writes_queue_.empty()) {
                            pollAndProcessCompletions();
                        }
                        
                        // Forward any frames still in the chunk buffer
                        for (auto* frame_buf : frame_chunk_buffer_) {
                            uint64_t frame_num = decoder_->get_super_frame_number(frame_buf);
                            forwardFrame(frame_buf, frame_num);
                            frames_forwarded_++;
                        }
                        frame_chunk_buffer_.clear();
                    }
                }

                if (!config_.enable_writing_)
                {
                    // Writing disabled - just forward frame immediately
                    forwardFrame(current_frame_buffer, frame_number);
                    frames_forwarded_++;
                    
                    uint64_t cycles_spent = rte_get_tsc_cycles() - start_frame_cycles;
                    total_frame_cycles += cycles_spent;
                    cycles_working += cycles_spent;
                    if (maximum_frame_cycles < cycles_spent)
                    {
                        maximum_frame_cycles = cycles_spent;
                    }
                    frames_per_second++;
                    processed_frames_++;
                }
                else if (tensorstore_initialized_ && store_.has_value() &&
                    pending_writes_queue_.size() < config_.max_concurrent_writes_)
                {
                    // Check if this frame or chunk would exceed max_frames
                    if (config_.max_frames_ > 0 && frame_number >= config_.max_frames_)
                    {
                        LOG4CXX_WARN(logger_, "Frame " << frame_number 
                            << " exceeds max_frames (" << config_.max_frames_ 
                            << "). Forwarding without writing.");
                        forwardFrame(current_frame_buffer, frame_number);
                        frames_forwarded_++;
                        
                        uint64_t cycles_spent = rte_get_tsc_cycles() - start_frame_cycles;
                        total_frame_cycles += cycles_spent;
                        cycles_working += cycles_spent;
                        if (maximum_frame_cycles < cycles_spent)
                        {
                            maximum_frame_cycles = cycles_spent;
                        }
                        frames_per_second++;
                        processed_frames_++;
                    }
                    else
                    {
                        // Add frame to chunk buffer
                        frame_chunk_buffer_.push_back(current_frame_buffer);

                        // Check if we have enough frames for a chunk
                        if (frame_chunk_buffer_.size() >= frames_per_chunk_)
                        {
                            // Prepare data pointers for all frames in chunk
                            std::vector<void*> raw_data_ptrs;
                            uint64_t first_frame_number = decoder_->get_super_frame_number(frame_chunk_buffer_[0]);
                            
                            for (auto* frame_buf : frame_chunk_buffer_)
                            {
                                raw_data_ptrs.push_back(decoder_->get_image_data_start(frame_buf));
                            }

                            const tensorstore::Index height = decoder_->get_frame_y_resolution();
                            const tensorstore::Index width = decoder_->get_frame_x_resolution();

                            tensorstore::WriteFutures write_future;
                            
                            // Write the chunk
                            switch(pixel_type_) {
                                case PixelDataType::UINT8:
                                    write_future = asyncWriteFrameChunk<uint8_t>(
                                        *store_, raw_data_ptrs, height, width, 
                                        first_frame_number, frame_chunk_buffer_.size()
                                    );
                                    break;
                                case PixelDataType::UINT16:
                                    write_future = asyncWriteFrameChunk<uint16_t>(
                                        *store_, raw_data_ptrs, height, width,
                                        first_frame_number, frame_chunk_buffer_.size()
                                    );
                                    break;
                                case PixelDataType::UINT32:
                                    write_future = asyncWriteFrameChunk<uint32_t>(
                                        *store_, raw_data_ptrs, height, width,
                                        first_frame_number, frame_chunk_buffer_.size()
                                    );
                                    break;
                                case PixelDataType::UINT64:
                                    write_future = asyncWriteFrameChunk<uint64_t>(
                                        *store_, raw_data_ptrs, height, width,
                                        first_frame_number, frame_chunk_buffer_.size()
                                    );
                                    break;
                                default:
                                    LOG4CXX_ERROR(logger_, "Unexpected pixel type in async write");
                                    continue;
                            }

                            
                            if (!first_write_recorded_) {
                                first_write_time_ = start_frame_cycles;
                                first_write_recorded_ = true;
                            }

                            // Add to pending queue (store all frame buffers)
                            pending_writes_queue_.push_back(PendingWrite{
                                .frame_number = first_frame_number,
                                .frame_buffers = frame_chunk_buffer_,
                                .write_future = std::move(write_future),
                                .start_cycles = start_frame_cycles
                            });

                            // Clear the chunk buffer
                            frame_chunk_buffer_.clear();

                            uint64_t cycles_spent = rte_get_tsc_cycles() - start_frame_cycles;
                            total_frame_cycles += cycles_spent;
                            cycles_working += cycles_spent;
                            if (maximum_frame_cycles < cycles_spent)
                            {
                                maximum_frame_cycles = cycles_spent;
                            }
                            frames_per_second += frames_per_chunk_;
                            processed_frames_ += frames_per_chunk_;

                            LOG4CXX_DEBUG_LEVEL(2, logger_, config_.core_name << " : " << proc_idx_ 
                                << " Initiated write for frame chunk starting at: " << first_frame_number
                                << " (Queue size: " << pending_writes_queue_.size() << ")"
                            );
                        }
                    }
                }
            }
        } // --- End Main Worker Loop ---

        LOG4CXX_INFO(logger_, "Core " << lcore_id_ << " completed");
        return true;
    }

    /**
     * Function: TensorstoreCore::pollAndProcessCompletions
     * Purpose: Checks the front of the queue to see if the oldest
     * write has completed. If so, processes it, forwards the frame,
     * and removes it from the queue
     */
    void TensorstoreCore::pollAndProcessCompletions()
    {
        if(!pending_writes_queue_.empty()) {
            // WriteFutures contains commit_future and copy_future
            auto& write_futures = pending_writes_queue_.front().write_future;
            LOG4CXX_DEBUG_LEVEL(2, logger_, "Pending write commit future status: " 
                << write_futures.commit_future.status());
        }

        // Check if the commit_future is ready (not the WriteFutures object itself)
        while (!pending_writes_queue_.empty() && 
               pending_writes_queue_.front().write_future.commit_future.ready())
        {
            PendingWrite& completed = pending_writes_queue_.front();
            
            // Calculate write time in microseconds
            uint64_t end_cycles = rte_get_tsc_cycles();
            uint64_t cycles_elapsed = end_cycles - completed.start_cycles;
            uint64_t write_time_us = (cycles_elapsed * 1000000) / rte_get_tsc_hz();
            
            // Get the result from commit_future
            const auto& result = completed.write_future.commit_future.result();
            if (!result.ok()) {
                ++write_errors_;
                LOG4CXX_ERROR(logger_, "Error writing frame chunk starting at " << completed.frame_number 
                    << " to " << config_.storage_driver_ << ": " << result.status());
                
                if (csv_logging_enabled_) {
                    logWriteToCSV(completed.frame_number, completed.frame_buffers.size(), 
                                 write_time_us, false);
                }
            } else {
                frames_written_ += completed.frame_buffers.size();
                ++completed_writes_;
                
                // Calculate rolling average in microseconds
                uint64_t total_write_time_us = avg_write_time_us_ * (completed_writes_ - 1);
                avg_write_time_us_ = (total_write_time_us + write_time_us) / completed_writes_;

                LOG4CXX_DEBUG_LEVEL (2, logger_, "Successfully wrote " << completed.frame_buffers.size()
                    << " frames starting at " << completed.frame_number
                    << " to " << config_.storage_driver_ << " in " << write_time_us << " us");
                
                if (csv_logging_enabled_) {
                    logWriteToCSV(completed.frame_number, completed.frame_buffers.size(), 
                                 write_time_us, true);
                }
            }

            // Forward all frame buffers downstream
            for (auto* frame_buf : completed.frame_buffers) {
                uint64_t frame_num = decoder_->get_super_frame_number(frame_buf);
                forwardFrame(frame_buf, frame_num);
                frames_forwarded_++;
            }

            // Remove the completed write from the front of the queue
            pending_writes_queue_.pop_front();
        }

        pending_writes_count_ = pending_writes_queue_.size();
    }

    /**
     * Function: TensorstoreCore::handleReconfiguration
     * Purpose: Handles the closing of the old store and creation of a new one
     */
    void TensorstoreCore::handleReconfiguration()
    {
        // Flush all pending writes (if a store exists)
        if (tensorstore_initialized_) 
        {
            LOG4CXX_INFO(logger_, "Reconfiguration requested. Waiting for " 
                << pending_writes_queue_.size() << " pending writes...");
            
            while (!pending_writes_queue_.empty()) {
                pollAndProcessCompletions();
            }

            LOG4CXX_INFO(logger_, "Write queue has cleared. Closing previous dataset.");

            store_.reset();
            tensorstore_initialized_ = false;
            frames_written_ = 0;
            write_errors_ = 0;
        }

        // Create the new dataset
        std::size_t height = config_.height_;
        std::size_t width  = config_.width_;
        std::size_t bit_depth = config_.bit_depth_;

        // Set frames_per_chunk from config
        frames_per_chunk_ = (config_.frames_per_chunk_ > 0) ? config_.frames_per_chunk_ : 1;
        
        LOG4CXX_INFO(logger_, "Creating new dataset with:"
            << " path=" << config_.path_
            << " width=" << width
            << " height=" << height
            << " bit_depth=" << bit_depth
            << " frames_per_chunk=" << frames_per_chunk_
        );

        // Determine pixel data type
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

        // Get the JSON spec
        std::size_t initial_frames = (config_.max_frames_ == 0) ? 1 : config_.max_frames_;
        ::nlohmann::json json_spec = GetJsonSpec(
            config_.storage_driver_, config_.kvstore_driver_, 
            config_.s3_bucket_, config_.s3_endpoint_,
            data_type_, config_.path_, 
            initial_frames, height, width, config_.cache_bytes_limit_,
            frames_per_chunk_
        );
        
        // Create the store
        auto store_result = create_dataset(json_spec);
        
        if (!store_result.ok()) {
            std::string error_msg = store_result.status().ToString();
        
            if (config_.kvstore_driver_ == "s3" && 
                error_msg.find("invalid scheme") != std::string::npos) {
                last_error_message_ = "S3 endpoint '" + config_.s3_endpoint_ + "' is missing http:// or https:// prefix. Please add the scheme to the endpoint URL.";
                LOG4CXX_ERROR(logger_, last_error_message_);
            }
            else if (config_.kvstore_driver_ == "s3" && (error_msg.find("permission_denied") != std::string::npos)) {
                last_error_message_ = "Cant connect to S3 endpoint '" + config_.s3_endpoint_ + "'. Please check your credentials and permissions.";
                LOG4CXX_ERROR(logger_, last_error_message_);
            }
            else if (error_msg.find("chunk_shape") != std::string::npos) {
                last_error_message_ = "Dataset already exists at '" + config_.path_ + "' with different chunk configuration. Please use a different path.";
                LOG4CXX_ERROR(logger_, last_error_message_);
            } 
            else if (error_msg.find("data_type") != std::string::npos) {
                last_error_message_ = "Dataset already exists at '" + config_.path_ + "' with different data type. Please use a different path.";
                LOG4CXX_ERROR(logger_, last_error_message_);
            }
            else if (error_msg.find("shape") != std::string::npos) {
                last_error_message_ = "Dataset already exists at '" + config_.path_ + "' with different dimensions. Please use a different path.";
                LOG4CXX_ERROR(logger_, last_error_message_);
            }
            else {
                last_error_message_ = "Failed to create/open dataset at '" + config_.path_ + "': " + error_msg;
                LOG4CXX_ERROR(logger_, last_error_message_);
            }
        } else {
            store_ = std::move(store_result.value());
            tensorstore_initialized_ = true;
            last_error_message_ = ""; 
            LOG4CXX_INFO(logger_, "Dataset created/opened successfully.");
        }
    }

    /**
     * Function: TensorstoreCore::forwardFrame
     * Purpose: Forwards a frame buffer to the correct downstream ring
     */
    void TensorstoreCore::forwardFrame(struct SuperFrameHeader* frame_buffer, uint64_t frame_number)
    {
        rte_ring_enqueue(
            // Select downstream ring in round-robin fashion based on frame number 
            downstream_rings_[frame_number % (config_.num_downstream_cores)], 
            frame_buffer
        );
    }

    /**
     * Function: TensorstoreCore::stop
     * Purpose: Stops the Tensorstore core's main loop
     */
    void TensorstoreCore::stop(void)
    {
        if (run_lcore_)
        {
            LOG4CXX_INFO(logger_, "Core " << lcore_id_ << " stopping");
            run_lcore_ = false;
        }
    }

    /**
     * Function: TensorstoreCore::status
     * Purpose: Reports the status of the Tensorstore core.
     */
    void TensorstoreCore::status(OdinData::IpcMessage& status, const std::string& path)
    {
        std::string status_path = path + "/TensorstoreCore_" + std::to_string(proc_idx_) + "/";
        std::string ring_status = status_path + "upstream_rings/";
        std::string timing_status = status_path + "timing/";
        std::string ts_status = status_path + "tensorstore/";

        // --- Frame Status ---
        status.set_param(status_path + "frames_dequeued", processed_frames_);
        status.set_param(status_path + "frames_forwarded", frames_forwarded_);
        status.set_param(status_path + "frames_processed_per_second", processed_frames_hz_);
        status.set_param(status_path + "idle_loops", idle_loops_);
        status.set_param(status_path + "core_usage", (int)core_usage_);
        status.set_param(status_path + "last_frame_number_dequeued", last_frame_);

        // --- Timing Status ---
        status.set_param(timing_status + "mean_frame_us", mean_us_on_frame_);
        status.set_param(timing_status + "max_frame_us", maximum_us_on_frame_);

        // --- Ring Status ---
        status.set_param(ring_status + ring_name_str(config_.upstream_core, socket_id_, proc_idx_) + "_count", rte_ring_count(upstream_ring_));
        status.set_param(ring_status + ring_name_str(config_.upstream_core, socket_id_, proc_idx_) + "_size", rte_ring_get_size(upstream_ring_));
        status.set_param(ring_status + ring_name_clear_frames(socket_id_) + "_count" , rte_ring_count(clear_frames_ring_));
        status.set_param(ring_status + ring_name_clear_frames(socket_id_) + "_size" , rte_ring_get_size(clear_frames_ring_));

        // --- TensorStore Status ---
        status.set_param(ts_status + "initialized", tensorstore_initialized_);
        status.set_param(ts_status + "storage_path", config_.path_);
        status.set_param(ts_status + "frames_written", frames_written_);
        status.set_param(ts_status + "write_errors", write_errors_);
        status.set_param(ts_status + "avg_write_time_us", avg_write_time_us_);
        status.set_param(ts_status + "pending_writes_queue_size", pending_writes_count_);
        status.set_param(ts_status + "total_completed_writes", completed_writes_);
        status.set_param(ts_status + "frames_per_chunk", frames_per_chunk_);
        status.set_param(ts_status + "enable_writing", config_.enable_writing_);
        status.set_param(ts_status + "last_error", last_error_message_);
    }

    /**
     * Function: TensorstoreCore::connect
     * Purpose: Connects to input (upstream) rings
     */
    bool TensorstoreCore::connect(void)
    {
        // --- Connect to upstream frame ring ---
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

        // --- Connect to clear frames ring ---
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

    /**
     * Function: TensorstoreCore::configure
     * Purpose: Configures the Tensorstore core
     */
    void TensorstoreCore::configure(OdinData::IpcMessage& config)
    {
        LOG4CXX_INFO(logger_, config_.core_name << " : " << proc_idx_ << " Got update config.");

        try {
        // Extract and update individual parameters from the IPC message
        // Note: These are returned from the odin-gui

        if (config.has_param("path"))
        {
            config_.path_ = config.get_param<std::string>("path");
            LOG4CXX_INFO(logger_, config_.core_name << " : " << proc_idx_ << " Setting path_ to: " <<  config_.path_);
        }

        if (config.has_param("storage_driver")) {
            config_.storage_driver_ = config.get_param<std::string>("storage_driver");
            LOG4CXX_INFO(logger_, config_.core_name << " : " << proc_idx_ << " Setting storage_driver_ to: " <<  config_.storage_driver_);
        }

        if (config.has_param("max_concurrent_writes")) {
            config_.max_concurrent_writes_ = config.get_param<int>("max_concurrent_writes");
            LOG4CXX_INFO(logger_, config_.core_name << " : " << proc_idx_ << " Setting max_concurrent_writes to: " <<  config_.max_concurrent_writes_);
        }

        if (config.has_param("frames_per_chunk")) {
            config_.frames_per_chunk_ = config.get_param<unsigned int>("frames_per_chunk");
            LOG4CXX_INFO(logger_, config_.core_name << " : " << proc_idx_ << " Setting frames_per_chunk to: " << config_.frames_per_chunk_);
        }

        if (config.has_param("enable_writing")) {
            bool previous_state = config_.enable_writing_;
            config_.enable_writing_ = config.get_param<bool>("enable_writing");
            
            if (config_.enable_writing_ != previous_state) {
                LOG4CXX_INFO(logger_, config_.core_name << " : " << proc_idx_ 
                    << " Setting enable_writing to: " << config_.enable_writing_);
                
                // If disabling writing, flush any pending writes
                if (!config_.enable_writing_ && tensorstore_initialized_) {
                    LOG4CXX_DEBUG_LEVEL(2, logger_, "Writing disabled. Flushing " 
                        << pending_writes_queue_.size() << " pending writes");
                    
                    while (!pending_writes_queue_.empty()) {
                        pollAndProcessCompletions();
                    }
                    
                    // Forward any frames still in the chunk buffer
                    for (auto* frame_buf : frame_chunk_buffer_) {
                        uint64_t frame_num = decoder_->get_super_frame_number(frame_buf);
                        forwardFrame(frame_buf, frame_num);
                        frames_forwarded_++;
                    }
                    frame_chunk_buffer_.clear();
                }
            }
        }

        if (config.has_param("path"))
        {
            config_.path_ = config.get_param<std::string>("path");
            LOG4CXX_INFO(logger_, config_.core_name << " : " << proc_idx_ << " Setting path_ to: " <<  config_.path_);
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


    /**
     * Function: DPDKREGISTER
     * Purpose: Registers this class with the DPDK plugin system
     */
    DPDKREGISTER(DpdkWorkerCore, TensorstoreCore, "TensorstoreCore");
    template tensorstore::WriteFutures TensorstoreCore::asyncWriteFrame<uint8_t>(
        tensorstore::TensorStore<>& store,
        void* raw_data,
        tensorstore::Index height,
        tensorstore::Index width,
        uint64_t frame_number
    );
    template tensorstore::WriteFutures TensorstoreCore::asyncWriteFrame<uint16_t>(
        tensorstore::TensorStore<>& store,
        void* raw_data,                 
        tensorstore::Index height,
        tensorstore::Index width,
        uint64_t frame_number
    );
    template tensorstore::WriteFutures TensorstoreCore::asyncWriteFrame<uint32_t>(
        tensorstore::TensorStore<>& store,
        void* raw_data,                 
        tensorstore::Index height,
        tensorstore::Index width,
        uint64_t frame_number
    );
    template tensorstore::WriteFutures TensorstoreCore::asyncWriteFrame<uint64_t>(
        tensorstore::TensorStore<>& store,
        void* raw_data,                 
        tensorstore::Index height,
        tensorstore::Index width,
        uint64_t frame_number
    );
    template tensorstore::WriteFutures TensorstoreCore::asyncWriteFrameChunk<uint8_t>(
        tensorstore::TensorStore<>& store, const std::vector<void*>& raw_data_ptrs,
        tensorstore::Index height, tensorstore::Index width,
        uint64_t start_frame_number, size_t num_frames
    );
    template tensorstore::WriteFutures TensorstoreCore::asyncWriteFrameChunk<uint16_t>(
        tensorstore::TensorStore<>& store, const std::vector<void*>& raw_data_ptrs,
        tensorstore::Index height, tensorstore::Index width,
        uint64_t start_frame_number, size_t num_frames
    );
    template tensorstore::WriteFutures TensorstoreCore::asyncWriteFrameChunk<uint32_t>(
        tensorstore::TensorStore<>& store, const std::vector<void*>& raw_data_ptrs,
        tensorstore::Index height, tensorstore::Index width,
        uint64_t start_frame_number, size_t num_frames
    );
    template tensorstore::WriteFutures TensorstoreCore::asyncWriteFrameChunk<uint64_t>(
        tensorstore::TensorStore<>& store, const std::vector<void*>& raw_data_ptrs,
        tensorstore::Index height, tensorstore::Index width,
        uint64_t start_frame_number, size_t num_frames
    );
    
    /**
     * Function: TensorstoreCore::openCSVLog
     * Purpose: Opens a CSV log file
     */
    void TensorstoreCore::openCSVLog(const std::string& filename)
    {
        csv_path_ = filename;
        csv_file_.open(csv_path_, std::ios::out | std::ios::trunc);
        
        if (csv_file_.is_open()) {
            csv_logging_enabled_ = true;
            
            // Write CSV header
            csv_file_ << "timestamp_seconds,frame_number,num_frames,write_time_us,"
                      << "success,cumulative_frames,avg_write_time_us,core_id,driver\n";
            csv_file_.flush();
            
            LOG4CXX_INFO(logger_, "CSV logging enabled: " << csv_path_);
        } else {
            csv_logging_enabled_ = false;
            LOG4CXX_ERROR(logger_, "Failed to open CSV log file: " << csv_path_);
        }
    }
    
    /**
     * Function: TensorstoreCore::logWriteToCSV
     * Purpose: Logs write performance data to the CSV file
     */
    void TensorstoreCore::logWriteToCSV(uint64_t frame_number, size_t num_frames, 
                                        uint64_t write_time_us, bool success)
    {
        if (!csv_file_.is_open()) {
            return;
        }
        
        // Calculates timestamp in seconds since first write
        uint64_t current_cycles = rte_get_tsc_cycles();
        double elapsed_seconds = static_cast<double>(current_cycles - first_write_time_) / 
                                static_cast<double>(rte_get_tsc_hz());
        
        // Write data row
        csv_file_ << elapsed_seconds << ","
                  << frame_number << ","
                  << num_frames << ","
                  << write_time_us << ","
                  << (success ? "1" : "0") << ","
                  << frames_written_ << ","
                  << avg_write_time_us_ << ","
                  << lcore_id_ << ","
                  << config_.storage_driver_ << "\n";
        csv_file_.flush();
    }
    
    /**
     * Function: TensorstoreCore::closeCSVLog
     * Purpose: Closes the CSV log file
     */
    void TensorstoreCore::closeCSVLog()
    {
        if (csv_file_.is_open()) {
            csv_file_.close();
            LOG4CXX_INFO(logger_, "CSV log file closed: " << csv_path_);
            csv_logging_enabled_ = false;
        }
    }
    
} // End of the FrameProcessor namespace
