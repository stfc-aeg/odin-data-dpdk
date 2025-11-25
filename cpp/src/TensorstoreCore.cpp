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
::nlohmann::json GetJsonSpec(const std::string& driver, const std::string& data_type, const std::string& path, std::size_t frames, std::size_t height, std::size_t width, uint64_t cache_bytes_limit) {
    return {
        {"driver", driver},
        {"kvstore", {
            {"driver", "file"},
            {"path", path}
        }},
        {"metadata", {
            {"data_type", data_type},
            {"shape", {frames, height, width}},
            {"chunk_grid", {
                {"name", "regular"},
                {"configuration", {
                    {"chunk_shape", {1, height, width}}
                }}
            }}
        }},
        {"context", {
            {"data_copy_concurrency", {
                {"limit", "shared"}
            }},
            {"cache_pool", {
                {"total_bytes_limit", cache_bytes_limit}
            }}
        }}
    };
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

    // 6. Select the target 2D slice, e.g., store[frame_number, :, :]
    std::array<tensorstore::Index, 1> indices = {target_frame_index};
    auto target = store | tensorstore::Dims(0).IndexSlice(indices);

    // 7. Write the 2D image data `array2` into the `target` slice
    LOG4CXX_DEBUG_LEVEL(2, logger_, "Calling write for frame " << frame_number);
    auto write_future = tensorstore::Write(array2, target);

    // 8. Return the future immediately.
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
    //move to config file
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
        completed_writes_(0)      
    {
        // Load configuration
        config_.resolve(dpdkWorkCoreReferences.core_config);
        config_.height_ = decoder_->get_frame_y_resolution();
        config_.width_  = decoder_->get_frame_x_resolution();
        config_.bit_depth_ = decoder_->get_frame_bit_depth();

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
        LOG4CXX_INFO(logger_, "TensorstoreCore: " << lcore_id_ << " starting up");

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

            // Dequeue New Frame and Initiate Write 
            if (tensorstore_initialized_ && store_.has_value() &&
                pending_writes_queue_.size() < config_.max_concurrent_writes_)
            {
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

                    
                    // --- Asynchronous TensorStore Write Logic ---
                    // We are guaranteed tensorstore_initialized_ is true here
                    char* raw_data = decoder_->get_image_data_start(current_frame_buffer);
                    const tensorstore::Index height = decoder_->get_frame_y_resolution();
                    const tensorstore::Index width = decoder_->get_frame_x_resolution();

                    tensorstore::WriteFutures write_future;
                
                    // Call the templated version of the async write using a switch statement based on data type
                    switch(pixel_type_) {
                        case PixelDataType::UINT8:
                            write_future = asyncWriteFrame<uint8_t>(
                                *store_, raw_data, height, width, frame_number
                            );
                            break;
                        case PixelDataType::UINT16:
                            write_future = asyncWriteFrame<uint16_t>(
                                *store_, raw_data, height, width, frame_number
                            );
                            break;
                        case PixelDataType::UINT32:
                            write_future = asyncWriteFrame<uint32_t>(
                                *store_, raw_data, height, width, frame_number
                            );
                            break;
                        case PixelDataType::UINT64:
                            write_future = asyncWriteFrame<uint64_t>(
                                *store_, raw_data, height, width, frame_number
                            );
                            break;
                        default:
                            LOG4CXX_ERROR(logger_, "Unexpected pixel type in async write");
                            continue;
                    }

                    // --- Add to Pending Queue ---
                    // Track the frame number along with its frame_buffer, write_future, and start_cycles
                    pending_writes_queue_.push_back(PendingWrite{
                        .frame_number = frame_number,
                        .frame_buffer = current_frame_buffer,
                        .write_future = std::move(write_future),
                        .start_cycles = start_frame_cycles
                    });

                    // --- Update Frame Dequeue Stats ---
                    uint64_t cycles_spent = rte_get_tsc_cycles() - start_frame_cycles;
                    total_frame_cycles += cycles_spent;
                    cycles_working += cycles_spent;
                    if (maximum_frame_cycles < cycles_spent)
                    {
                        maximum_frame_cycles = cycles_spent;
                    }
                    frames_per_second++;
                    processed_frames_++; // Counter to show frames number of processed frames

                    LOG4CXX_DEBUG_LEVEL(2, logger_, config_.core_name << " : " << proc_idx_ 
                        << " Initiated write for frame: " << frame_number
                        << " (Queue size: " << pending_writes_queue_.size() << ")"
                    );
                }
            }
            else if (!tensorstore_initialized_)
            {
                // If tensorestore is not initialized e.g. on startup, before first config
                // or if reconfiguring is occuring then  nothing happens and it is counted
                // as an idle loop.
                idle_loops++;
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
                LOG4CXX_ERROR(logger_, "Error writing frame " << completed.frame_number 
                    << " to " << config_.driver_ << ": " << result.status());
            } else {
                ++frames_written_;
                ++completed_writes_;
                
                // Calculate rolling average in microseconds
                uint64_t total_write_time_us = avg_write_time_us_ * (completed_writes_ - 1);
                avg_write_time_us_ = (total_write_time_us + write_time_us) / completed_writes_;
                
                LOG4CXX_DEBUG_LEVEL(2, logger_, "Successfully wrote frame " << completed.frame_number 
                    << " to " << config_.driver_ << " in " << write_time_us << " us");
            }

            // Forward the frame buffer downstream
            forwardFrame(completed.frame_buffer, completed.frame_number);
            frames_forwarded_++;

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
            
            // This loop will block this thread (but not other cores)
            // until all outstanding writes are finished.
            while (!pending_writes_queue_.empty()) {
                pollAndProcessCompletions();
            }

            LOG4CXX_INFO(logger_, "Write queue has cleared. Closing previous dataset.");

            // Resets the store to close it.
            store_.reset();
            tensorstore_initialized_ = false;
            frames_written_ = 0;
            write_errors_ = 0;
        }

        // Create the new dataset

        // Use the configuration values set by configure()
        std::size_t height = config_.height_;
        std::size_t width  = config_.width_;
        std::size_t bit_depth = config_.bit_depth_;

        LOG4CXX_INFO(logger_, "Creating new dataset with:"
            << " width=" << width
            << " height=" << height
            << " bit_depth=" << bit_depth
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
                return;
        }

        // Get the JSON spec
        // Always create dataset with at least 1 frame to avoid immediate resize
        std::size_t initial_frames = (config_.max_frames_ == 0) ? 1 : config_.max_frames_;
        ::nlohmann::json json_spec = GetJsonSpec(config_.driver_, data_type_, config_.file_path_, initial_frames, height, width, config_.cache_bytes_limit_);
        
        // Create the store
        auto store_result = create_dataset(json_spec);
        
        if (!store_result.ok()) {
            // The initialized flag is not set if there is an error
            LOG4CXX_ERROR(logger_, "Error creating " << config_.driver_ << " file: " << store_result.status());
        } else {
            // The store has been successfully created and the initialized flag is set to true
            store_ = std::move(store_result.value());
            tensorstore_initialized_ = true;
            LOG4CXX_INFO(logger_, "New dataset created successfully. Resuming operations.");
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
        status.set_param(ts_status + "storage_path", config_.file_path_);
        status.set_param(ts_status + "frames_written", frames_written_);
        status.set_param(ts_status + "write_errors", write_errors_);
        status.set_param(ts_status + "avg_write_time_us", avg_write_time_us_);
        status.set_param(ts_status + "pending_writes_queue_size", pending_writes_count_);
        status.set_param(ts_status + "total_completed_writes", completed_writes_);
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

        if (config.has_param("file_path"))
        {
            config_.file_path_ = config.get_param<std::string>("file_path");
            LOG4CXX_INFO(logger_, config_.core_name << " : " << proc_idx_ << " Setting file_path_ to: " <<  config_.file_path_);
        }

        if (config.has_param("driver")) {
            config_.driver_ = config.get_param<std::string>("driver");
            LOG4CXX_INFO(logger_, config_.core_name << " : " << proc_idx_ << " Setting driver_ to: " <<  config_.driver_);
        }

        if (config.has_param("max_concurrent_writes")) {
            config_.max_concurrent_writes_ = config.get_param<int>("max_concurrent_writes");
            LOG4CXX_INFO(logger_, config_.core_name << " : " << proc_idx_ << " Setting max_concurrent_writes to: " <<  config_.max_concurrent_writes_);
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
} // End of the FrameProcessor namespace
