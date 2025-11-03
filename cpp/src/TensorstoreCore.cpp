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

// Enum for pixel data types
enum class PixelDataType {
    UINT8,
    UINT16,
    UINT32,
    UINT64
};


////////////////////////////////////////////////////////////////////////////////
// SECTION 3: STORAGE CONFIGURATION FUNCTION
////////////////////////////////////////////////////////////////////////////////

/**
 * Function: GetJsonSpec
 * Purpose: Creates a "JSON" object that defines the complete storage layout
 * for TensorStore (driver, path, data type, shape, chunking).
 */
::nlohmann::json GetJsonSpec(const std::string& data_type, int frames, int height, int width) {
    return {
        {"driver", "zarr3"},
        {"kvstore", {
            {"driver", "file"},
            {"path", "/tmp"} // Note to self: Make this configurable
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
                {"total_bytes_limit", 17179869184} // Note to self: Make this configurable
            }}
        }}
    };
}

////////////////////////////////////////////////////////////////////////////////
// SECTION 4: STORAGE CREATION AND ASYNC WRITE FUNCTIONS
////////////////////////////////////////////////////////////////////////////////

/**
 * Function: create_zarr
 * Purpose: Takes the JSON settings and creates/opens the TensorStore.
 * This is a blocking call which meansit is intended to be run only once at initialization.
 */
static tensorstore::Result<tensorstore::TensorStore<>> create_zarr(const ::nlohmann::json& json_spec) {
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

    // 2. Describes the 2D image's memory layout (shape and strides).
    std::array<tensorstore::Index, 2> shape = {width, height};
    std::array<tensorstore::Index, 2> byte_strides = {
        width * static_cast<tensorstore::Index>(sizeof(T)),
        static_cast<tensorstore::Index>(sizeof(T))
    };
    tensorstore::StridedLayout<2> layout(shape, byte_strides);

    // 3. Create a "view" of the data (zero-copy).
    tensorstore::ArrayView<T, 2> two_dim_view(data_as_T, layout);

    // 4. Convert the view to a "shared" array for safe writing.
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
        
        // Execute the resize operation (BLOCKING).
        auto resize_status = tensorstore::Resize(
            store,
            inclusive_min_span,
            exclusive_max_span
        ).result();

    }

    // 6. Select the target 2D slice, e.g., store[frame_number, :, :].
    std::array<tensorstore::Index, 1> indices = {target_frame_index};
    auto target = store | tensorstore::Dims(0).IndexSlice(indices);

    // 7. Write the 2D image data `array2` into the `target` slice.
    // This call is NON-BLOCKING and returns a Future.
    LOG4CXX_INFO(logger_, "Calling write for frame " << frame_number);
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
        frames_forwarded_(0)
    {
        // Load configuration
        config_.resolve(dpdkWorkCoreReferences.core_config);

        LOG4CXX_INFO(logger_, "FP.TensorstoreCore " << proc_idx_ << " Created with config:"
            << " core_name=" << config_.core_name
            << " connect=" << config_.connect
            << " upstream_core=" << config_.upstream_core
            << " num_cores=" << config_.num_cores
            << " num_downstream_cores=" << config_.num_downstream_cores
            << " storage_path=" << config_.storage_path_
            << " max_frames=" << config_.max_frames_
            << " frame_size=" << config_.frame_size_
            << " chunk_size=" << config_.chunk_size_
            << " cache_bytes_limit=" << config_.cache_bytes_limit_
            << " data_copy_concurrency=" << config_.data_copy_concurrency_
            << " delete_existing=" << config_.delete_existing_
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
     * Purpose: The main loop for the worker core.
     *
     * This is a 2-part loop:
     * 1. We poll for completed writes and forward them (FIFO).
     * 2. Then we dequeue new frames from upstream and initiate asynchronous writes.
     */
    bool TensorstoreCore::run(unsigned int lcore_id)
    {
        lcore_id_ = lcore_id;
        run_lcore_ = true;
        LOG4CXX_INFO(logger_, "TensorstoreCore: " << lcore_id_ << " starting up");
        LOG4CXX_INFO(logger_, "Core ID: " << lcore_id_);

        struct SuperFrameHeader *current_frame_buffer;
        LOG4CXX_INFO(logger_, "Current Frame Buffer: " << current_frame_buffer);

        // Performance monitoring variables
        uint64_t frames_per_second = 1;
        uint64_t total_frame_cycles = 1;
        uint64_t cycles_working = 1;
        uint64_t maximum_frame_cycles = 0;
        uint64_t idle_loops = 0;
        uint64_t start_frame_cycles = 0;
        uint64_t last = rte_get_tsc_cycles();
        uint64_t cycles_per_sec = rte_get_tsc_hz();
        LOG4CXX_INFO(logger_, "Core ID: " << lcore_id_);

        LOG4CXX_INFO(logger_, "Frames Per Second: " << frames_per_second);
        LOG4CXX_INFO(logger_, "Total Frame Cycles: " << total_frame_cycles);
        LOG4CXX_INFO(logger_, "Cycles Working: " << cycles_working);
        LOG4CXX_INFO(logger_, "Maximum Frame Cycles: " << maximum_frame_cycles);
        LOG4CXX_INFO(logger_, "Idle Loops: " << idle_loops);

        // --- Main Worker Loop ---
        while (likely(run_lcore_)) 
        {
            // --- Performance Stats Update (runs approx once per second) ---
            uint64_t now = rte_get_tsc_cycles();
            if (unlikely((now - last) >= (cycles_per_sec))) //unlikely means this condition is rarely true
            {
                processed_frames_hz_ = frames_per_second - 1;
                mean_us_on_frame_ = (total_frame_cycles * 1000000) / (frames_per_second * cycles_per_sec);
                core_usage_ = (cycles_working * 255) / cycles_per_sec;
                maximum_us_on_frame_ = (maximum_frame_cycles * 1000000) / (cycles_per_sec);
                idle_loops_ = idle_loops;

                // Reset per-second counters.
                frames_per_second = 1;
                idle_loops = 0;
                total_frame_cycles = 1;
                cycles_working = 1;
                last = now;
            }
            
            // --- PART 1: Poll for and Process Completed Writes (FIFO) ---
            // This function checks the front of the pending_writes_queue_
            // and processes any completed writes.
            pollAndProcessCompletions();

            // --- PART 2: Dequeue New Frame and Initiate Write ---
            if (rte_ring_dequeue(upstream_ring_, (void**) &current_frame_buffer) < 0)
            {
                // If the ring is empty, count it as an "idle" loop and try again.
                // We don't continue here, because we still need to
                // check for completed writes even if no new frames arrive.
                idle_loops++;
            }
            else // Frame has been dequeued successfully
            {
                start_frame_cycles = rte_get_tsc_cycles();
                uint64_t frame_number = decoder_->get_super_frame_number(current_frame_buffer);
                LOG4CXX_INFO(logger_, "Dequeuing frame: " << frame_number);
                last_frame_ = frame_number;

                // --- First-Frame-Only Initialization ---
                if (!tensorstore_initialized_) {
                    int width  = static_cast<int>(decoder_->get_frame_y_resolution());
                    int height = static_cast<int>(decoder_->get_frame_x_resolution());
                    int bit_depth = static_cast<int>(decoder_->get_frame_bit_depth());

                    LOG4CXX_INFO(logger_, "Width: " << width);
                    LOG4CXX_INFO(logger_, "Height: " << height);
                    LOG4CXX_INFO(logger_, "Bit Depth: " << bit_depth);

                    // Determine pixel data type based on bit depth using a switch case statement
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
                            forwardFrame(current_frame_buffer, frame_number);
                            continue;
                    }

                    

                    ::nlohmann::json json_spec = GetJsonSpec(data_type_, config_.max_frames_, height, width);
                    
                    auto store_result = create_zarr(json_spec);
                    
                    if (!store_result.ok()) {
                        LOG4CXX_ERROR(logger_, "Error creating Zarr file: " << store_result.status());
                        //  Don't set initialized flag since store creation has failed
                    } else {
                        // Successfully created the store
                        store_ = std::move(store_result.value());
                        tensorstore_initialized_ = true;
                    }
                }
                
                // --- Asynchronous TensorStore Write Logic ---
                 if (tensorstore_initialized_) {
                    char* raw_data = decoder_->get_image_data_start(current_frame_buffer);
                    const tensorstore::Index height = decoder_->get_frame_y_resolution();
                    const tensorstore::Index width = decoder_->get_frame_x_resolution();

                    tensorstore::WriteFutures write_future;
                
                //     // Call the templated version of the async write using a switch statement based on data type
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
                            forwardFrame(current_frame_buffer, frame_number);
                            continue;
                    }

                    // --- Add to Pending Queue ---
                    // This helps to track the frame number along with its
                    // frame_buffer, write_future, and start_cycles.
                    pending_writes_queue_.push_back(PendingWrite{
                        .frame_number = frame_number,
                        .frame_buffer = current_frame_buffer,
                        .write_future = std::move(write_future),
                        .start_cycles = start_frame_cycles
                    });

                } else {
                    // Store failed to open, count as an error and forward.
                    ++write_errors_;
                    forwardFrame(current_frame_buffer, frame_number);
                 }

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

                LOG4CXX_INFO(logger_, config_.core_name << " : " << proc_idx_ 
                    << " Initiated write for frame: " << frame_number
                    << " (Queue size: " << pending_writes_queue_.size() << ")"
                );
            }
        } // --- End Main Worker Loop ---

        LOG4CXX_INFO(logger_, "Core " << lcore_id_ << " completed");
        return true;
    }

    /**
     * Polls the queue of pending writes.
     * Checks the front of the queue (FIFO) to see if the oldest
     * write has completed. If so, processes it, forwards the frame,
     * and removes it from the queue.
     */
    void TensorstoreCore::pollAndProcessCompletions()
    {
        if(!pending_writes_queue_.empty()) {
            auto hbgeduhybfdsuyhf = pending_writes_queue_.front().write_future.status();
            LOG4CXX_INFO(logger_, "Pending write future status: " << hbgeduhybfdsuyhf);

        }
        // auto hbgeduhybfdsuyhf = pending_writes_queue_.front().write_future.status();
        // This checks the oldest pending write. 
        while (!pending_writes_queue_.empty() && 
               pending_writes_queue_.front().write_future.result())
        {
            // Get the completed write info from the front of the queue
            PendingWrite& completed = pending_writes_queue_.front();
            
            // Check the status of the completed future
            const auto& result = completed.write_future.result();
            if (!result.ok()) {
                ++write_errors_;
                LOG4CXX_ERROR(logger_, "Error writing frame " << completed.frame_number 
                    << " to Zarr: " << result.status());
            } else {
                ++frames_written_;
                LOG4CXX_INFO(logger_, "Successfully wrote frame " << completed.frame_number << " to Zarr.");
            }

            // Forward the frame buffer downstream
            forwardFrame(completed.frame_buffer, completed.frame_number);
            frames_forwarded_++;

            // Remove the completed write from the front of the queue
            pending_writes_queue_.pop_front();
        }

        // Update status counter
        pending_writes_count_ = pending_writes_queue_.size();
    }

    /**
     * Forwards a frame buffer to the correct downstream ring.
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
        // std::string status_path = path + "/TensorstoreCore_" + std::to_string(proc_idx_) + "/";
        // std::string ring_status = status_path + "upstream_rings/";
        // std::string timing_status = status_path + "timing/";
        // std::string ts_status = status_path + "tensorstore/";

        // // --- Frame Status ---
        // status.set_param(status_path + "frames_dequeued", processed_frames_);
        // status.set_param(status_path + "frames_forwarded", frames_forwarded_);
        // status.set_param(status_path + "frames_processed_per_second", processed_frames_hz_);
        // status.set_param(status_path + "idle_loops", idle_loops_);
        // status.set_param(status_path + "core_usage", (int)core_usage_);
        // status.set_param(status_path + "last_frame_number_dequeued", last_frame_);

        // // --- Timing Status (measures dequeue/initiation, not full write) ---
        // status.set_param(timing_status + "mean_frame_us", mean_us_on_frame_);
        // status.set_param(timing_status + "max_frame_us", maximum_us_on_frame_);

        // // --- Ring Status ---
        // status.set_param(ring_status + ring_name_str(config_.upstream_core, socket_id_, proc_idx_) + "_count", rte_ring_count(upstream_ring_));
        // status.set_param(ring_status + ring_name_str(config_.upstream_core, socket_id_, proc_idx_) + "_size", rte_ring_get_size(upstream_ring_));
        // status.set_param(ring_status + ring_name_clear_frames(socket_id_) + "_count" , rte_ring_count(clear_frames_ring_));
        // status.set_param(ring_status + ring_name_clear_frames(socket_id_) + "_size" , rte_ring_get_size(clear_frames_ring_));

        // // --- TensorStore Status (now includes async stats) ---
        // status.set_param(ts_status + "initialized", tensorstore_initialized_);
        // status.set_param(ts_status + "storage_path", storage_path_); // Note to self: this variable is currently not set
        // status.set_param(ts_status + "frames_written", frames_written_);
        // status.set_param(ts_status + "write_errors", write_errors_);
        // status.set_param(ts_status + "avg_write_time_us", avg_write_time_us_); // Note to self: This variable is currently not calculated
        // status.set_param(ts_status + "pending_writes_queue_size", pending_writes_count_);
    }

    /**
     * Function: TensorstoreCore::connect
     * Purpose: Connects to input (upstream) rings.
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
    }

    /**
     * Function: DPDKREGISTER
     * Purpose: Registers this class with the DPDK plugin system.
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
