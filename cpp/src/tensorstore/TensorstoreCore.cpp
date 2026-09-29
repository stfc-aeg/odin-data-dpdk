#include "tensorstore/TensorstoreCore.h"

#include <stdexcept>
#include "DpdkUtils.h"
#include "camera/CameraController.h"

#include "tensorstore/TensorstoreJsonSpec.h"
#include "tensorstore/TensorstoreDataset.h"
#include "tensorstore/TensorstoreWriter.h"
#include "tensorstore/TensorstoreResizer.h"
#include "tensorstore/TensorstorePerformanceMonitor.h"
#include "tensorstore/TensorstoreErrorHandler.h"
#include "tensorstore/TensorstoreFlushManager.h"

#include <tensorstore/index_space/dim_expression.h>

#include <string>
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
    TensorstoreCore::TensorstoreCore(
        int fb_idx, int socket_id, DpdkWorkCoreReferences &dpdkWorkCoreReferences
    ) :
        DpdkWorkerCore(socket_id), 
        logger_(Logger::getLogger("FP.TensorstoreCore")),
        proc_idx_(fb_idx),
        mode_(dpdkWorkCoreReferences.decoder_mode),
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
        frames_per_second_(0),
        dimension_mismatch_logged_(false)
    {
        config_.resolve(dpdkWorkCoreReferences.core_config, dpdkWorkCoreReferences.config_key);

        // An explicit height/width in the config block wins, so that a stream sitting downstream
        // of a core which resizes frames (e.g. RegionOfInterestCore) can declare the post-resize
        // dims. Where they are absent, fall back to the decoder mode's native resolution.
        bool height_from_config = (config_.height_ != 0);
        bool width_from_config  = (config_.width_ != 0);
        if (config_.height_ == 0)
        {
            config_.height_ = decoder_->get_frame_y_resolution(mode_);
        }
        if (config_.width_ == 0)
        {
            config_.width_ = decoder_->get_frame_x_resolution(mode_);
        }
        config_.bit_depth_ = decoder_->get_frame_bit_depth(mode_);

        // enable_writing always starts false; set true by update_config to start an acquisition
        config_.enable_writing_ = false;


        LOG4CXX_INFO(logger_, "FP.TensorstoreCore " << proc_idx_ << " Created with config:"
            << " core_name = " << config_.core_name
            << " connect = " << config_.connect
            << " upstream_core = " << config_.upstream_core
            << " config_key = " << config_.config_key
            << " stream_id = " << config_.stream_id
            << " mode = " << mode_
            << " height = " << config_.height_
            << " (from " << (height_from_config ? "config" : "decoder_mode") << ")"
            << " width = " << config_.width_
            << " (from " << (width_from_config ? "config" : "decoder_mode") << ")"
            << " bit_depth = " << config_.bit_depth_
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
            << " path = " << streamScopedDatasetPath()
            << " csv_logging = " << config_.csv_logging_
            << " csv_path = " << csv_path_
        );

        // Create downstream rings, or look them up if already created by a sibling core
        for (int ring_idx = 0; ring_idx < config_.num_downstream_cores; ring_idx++)
        {
            std::string downstream_ring_name = ring_name_str(config_.config_key, socket_id_, ring_idx);
            struct rte_ring* downstream_ring = rte_ring_lookup(downstream_ring_name.c_str());

            if (downstream_ring == NULL)
            {
                unsigned int downstream_ring_size = nearest_power_two(shared_buf_->get_num_buffers() * 2);
                LOG4CXX_INFO(logger_, "Creating ring name "
                    << downstream_ring_name << " of size " << downstream_ring_size
                );
                downstream_ring = rte_ring_create(
                    downstream_ring_name.c_str(), downstream_ring_size, socket_id_, 0
                );
                if (downstream_ring == NULL)
                {
                    throw std::runtime_error("Failed to create downstream ring " + downstream_ring_name
                        + ": " + rte_strerror(rte_errno));
                }
            }

            downstream_rings_.push_back(downstream_ring);
        }
    }

    std::string TensorstoreCore::streamScopedDatasetPath() const
    {
        // Derived on demand rather than written back into config_.path_, so that a configure()
        // message setting "path" cannot silently drop the stream scoping, and so that repeated
        // calls cannot append the stream twice.
        if (config_.stream_id.empty())
        {
            return config_.path_;
        }

        std::string scoped = config_.path_;
        if (!scoped.empty() && scoped.back() != '/')
        {
            scoped += '/';
        }
        scoped += config_.stream_id + "/";
        return scoped;
    }

    std::string TensorstoreCore::streamScopedCsvPath() const
    {
        // Insert the stream before the extension, so that the per-acquisition timestamp
        // insertion in handleReconfiguration still finds the extension and appends after it.
        if (config_.stream_id.empty() || config_.csv_path_.empty())
        {
            return config_.csv_path_;
        }

        std::string csv = config_.csv_path_;
        std::size_t dot_pos = csv.find_last_of('.');
        std::size_t slash_pos = csv.find_last_of('/');
        // Only treat the dot as an extension if it comes after the final path separator
        if (dot_pos != std::string::npos &&
            (slash_pos == std::string::npos || dot_pos > slash_pos))
        {
            csv.insert(dot_pos, "_" + config_.stream_id);
        }
        else
        {
            csv += "_" + config_.stream_id;
        }
        return csv;
    }

    std::string TensorstoreCore::datasetDestinationDescription() const
    {
        const std::string path = streamScopedDatasetPath();

        if (config_.kvstore_driver_ == "s3")
        {
            // For s3 the path is a key prefix within the bucket, so name the bucket and endpoint
            // as well; the prefix on its own tells the reader nothing about which store it is in.
            std::string desc = "s3 bucket '" + config_.s3_bucket_ + "' key prefix '" + path + "'";
            if (!config_.s3_endpoint_.empty())
            {
                desc += " at " + config_.s3_endpoint_;
            }
            return desc;
        }

        if (config_.kvstore_driver_ == "file")
        {
            return "local directory '" + path + "'";
        }

        return config_.kvstore_driver_ + " store '" + path + "'";
    }

    bool TensorstoreCore::frameDimensionsMatchDataset(
        ::SuperFrameHeader* frame_buffer, uint64_t frame_number
    )
    {
        uint64_t frame_height = decoder_->get_super_frame_y_resolution(frame_buffer);
        uint64_t frame_width  = decoder_->get_super_frame_x_resolution(frame_buffer);

        // Zero means no upstream core stamped the actual dimensions into the header, so there is
        // nothing to check against - trust the configured dims.
        if (frame_height == 0 || frame_width == 0)
        {
            return true;
        }

        if (frame_height == config_.height_ && frame_width == config_.width_)
        {
            return true;
        }

        if (!dimension_mismatch_logged_)
        {
            dimension_mismatch_logged_ = true;
            LOG4CXX_ERROR(logger_, "Frame " << frame_number << " dimensions ("
                << frame_width << "x" << frame_height << ") do not match the dataset dimensions ("
                << config_.width_ << "x" << config_.height_ << ") for stream '"
                << config_.stream_id << "' mode '" << mode_ << "'."
                << " Writing it would corrupt the dataset, so this frame and any further"
                << " mismatched frames this acquisition will be forwarded without being written."
                << " Set \"height\" and \"width\" in the '" << config_.config_key
                << "' config block to the dimensions actually produced upstream.");
            last_error_message_ = "Frame dimensions " + std::to_string(frame_width) + "x"
                + std::to_string(frame_height) + " do not match dataset "
                + std::to_string(config_.width_) + "x" + std::to_string(config_.height_);
        }

        return false;
    }

    TensorstoreCore::~TensorstoreCore(void)
    {
        LOG4CXX_DEBUG_LEVEL(2, logger_, "TensorstoreCore destructor");
        csv_logger_.Close(logger_);
        stop();
    }

    bool TensorstoreCore::run(unsigned int lcore_id)
    {
        lcore_id_ = lcore_id;
        run_lcore_ = true;
        run_start_time_ = rte_get_tsc_cycles();
        LOG4CXX_INFO(logger_, "TensorstoreCore: " << lcore_id_ << " starting up");

        ::SuperFrameHeader *current_frame_buffer;
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
                    config_.height_,
                    config_.width_,
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

                if (!config_.enable_writing_)
                {
                    forwardFrame(current_frame_buffer, frame_number);
                    frames_forwarded_++;

                    uint64_t cycles_spent = rte_get_tsc_cycles() - start_frame_cycles;
                    perf_monitor_.RecordFrameProcessing(cycles_spent);
                    perf_monitor_.FramesThisSecond()++;
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
                    const uint64_t frames_in_super_frame = decoder_->get_frame_outer_chunk_size(mode_);
                    const uint64_t starting_frame_index = frame_number * frames_in_super_frame;
                    const uint64_t ending_frame_index = starting_frame_index + frames_in_super_frame - 1;
                    
                    if (config_.number_of_frames_ > 0 && starting_frame_index >= config_.number_of_frames_)
                    {
                        LOG4CXX_WARN(logger_, "Processed all frames from acquisition. (" << config_.number_of_frames_ 
                            << "). Disabling writing. Subsequent frames will be forwarded.");
                        
                        config_.enable_writing_ = false;
                        
                        LOG4CXX_INFO(logger_, "Flushing " << pending_writes_queue_.size() 
                            << " pending writes...");
                        flush_pending_writes = true;
                        
                        forwardFrame(current_frame_buffer, frame_number);
                        frames_forwarded_++;
                        
                        uint64_t cycles_spent = rte_get_tsc_cycles() - start_frame_cycles;
                        perf_monitor_.RecordFrameProcessing(cycles_spent);
                        perf_monitor_.FramesThisSecond()++;
                    }
                    else if (!frameDimensionsMatchDataset(current_frame_buffer, frame_number))
                    {
                        // Geometry disagrees with the open dataset; forward rather than corrupt it
                        forwardFrame(current_frame_buffer, frame_number);
                        frames_forwarded_++;

                        uint64_t cycles_spent = rte_get_tsc_cycles() - start_frame_cycles;
                        perf_monitor_.RecordFrameProcessing(cycles_spent);
                        perf_monitor_.FramesThisSecond()++;
                    }
                    else
                    {

                        if (TensorstoreResizer::NeedsExpansion(ending_frame_index, current_dataset_capacity_)) {
                            const tensorstore::Index height = config_.height_;
                            const tensorstore::Index width = config_.width_;

                            current_dataset_capacity_ = TensorstoreResizer::ExpandDataset(
                                *store_,
                                current_dataset_capacity_,
                                kDatasetExpansionIncrement,
                                height,
                                width,
                                logger_
                            );
                        }

                        if (ending_frame_index > highest_frame_written_) {
                            highest_frame_written_ = ending_frame_index;
                        }

                        void* frame_ptr = static_cast<void*>(
                            decoder_->get_image_data_start(current_frame_buffer, mode_));
                        const tensorstore::Index height = config_.height_;
                        const tensorstore::Index width = config_.width_;

                        tensorstore::WriteFutures write_future;
                        bool valid_pixel_type = true;

                        if (frames_in_super_frame > 1) {
                            LOG4CXX_DEBUG_LEVEL(2, logger_, "Writing chunk: super_frame=" << frame_number 
                                << " frames=" << frames_in_super_frame 
                                << " starting_index=" << starting_frame_index);
                            
                            switch(pixel_type_) {
                                case PixelDataType::UINT8:
                                    write_future = TensorstoreWriter::AsyncWriteFrameChunk<uint8_t>(
                                        *store_, frame_ptr, frames_in_super_frame, height, width,
                                        starting_frame_index, logger_
                                    );
                                    break;
                                case PixelDataType::UINT16:
                                    write_future = TensorstoreWriter::AsyncWriteFrameChunk<uint16_t>(
                                        *store_, frame_ptr, frames_in_super_frame, height, width,
                                        starting_frame_index, logger_
                                    );
                                    break;
                                case PixelDataType::UINT32:
                                    write_future = TensorstoreWriter::AsyncWriteFrameChunk<uint32_t>(
                                        *store_, frame_ptr, frames_in_super_frame, height, width,
                                        starting_frame_index, logger_
                                    );
                                    break;
                                case PixelDataType::UINT64:
                                    write_future = TensorstoreWriter::AsyncWriteFrameChunk<uint64_t>(
                                        *store_, frame_ptr, frames_in_super_frame, height, width,
                                        starting_frame_index, logger_
                                    );
                                    break;
                                default:
                                    LOG4CXX_ERROR(logger_, "Unexpected pixel type in async write for frames " 
                                        << starting_frame_index << "-" << ending_frame_index 
                                        << ". Forwarding without writing.");
                                    valid_pixel_type = false;
                                    break;
                            }
                        } else {
                            switch(pixel_type_) {
                                case PixelDataType::UINT8:
                                    write_future = TensorstoreWriter::AsyncWriteFrame<uint8_t>(
                                        *store_, frame_ptr, height, width,
                                        starting_frame_index, logger_
                                    );
                                    break;
                                case PixelDataType::UINT16:
                                    write_future = TensorstoreWriter::AsyncWriteFrame<uint16_t>(
                                        *store_, frame_ptr, height, width,
                                        starting_frame_index, logger_
                                    );
                                    break;
                                case PixelDataType::UINT32:
                                    write_future = TensorstoreWriter::AsyncWriteFrame<uint32_t>(
                                        *store_, frame_ptr, height, width,
                                        starting_frame_index, logger_
                                    );
                                    break;
                                case PixelDataType::UINT64:
                                    write_future = TensorstoreWriter::AsyncWriteFrame<uint64_t>(
                                        *store_, frame_ptr, height, width,
                                        starting_frame_index, logger_
                                    );
                                    break;
                                default:
                                    LOG4CXX_ERROR(logger_, "Unexpected pixel type in async write for frame " 
                                        << starting_frame_index << ". Forwarding without writing.");
                                    valid_pixel_type = false;
                                    break;
                            }
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
                            .frame_number = starting_frame_index,
                            .frame_buffers = {current_frame_buffer},
                            .write_future = std::move(write_future),
                            .start_cycles = start_frame_cycles,
                            .num_frames = frames_in_super_frame
                        };
                        pending_writes_queue_.insert({starting_frame_index, std::move(pw)});

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
                        pending.num_frames,
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
                    frames_written_ += pending.num_frames;
                    ++completed_writes_;
                    // Incremental running average to avoid overflow
                    uint64_t total_write_time_us = avg_write_time_us_ * (completed_writes_ - 1);
                    avg_write_time_us_ = (total_write_time_us + write_time_us) / completed_writes_;
                    LOG4CXX_DEBUG_LEVEL (2, logger_, "Successfully wrote " << pending.num_frames
                        << " frames starting at " << pending.frame_number
                        << " to " << config_.storage_driver_ << " in " << write_time_us << " us");
                    csv_logger_.LogWrite(
                        pending.frame_number,
                        pending.num_frames,
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

    void TensorstoreCore::handleReconfiguration()
    {
        if (tensorstore_initialized_)
        {
            LOG4CXX_INFO(logger_, "Reconfiguration: flushing " << pending_writes_queue_.size() << " pending writes");

            while (!pending_writes_queue_.empty())
            {
                pollAndProcessCompletions();
            }

            for (auto* frame_buf : frame_chunk_buffer_)
            {
                uint64_t frame_num = decoder_->get_super_frame_number(frame_buf);
                forwardFrame(frame_buf, frame_num);
                frames_forwarded_++;
            }
            frame_chunk_buffer_.clear();

            store_.reset();
            tensorstore_initialized_ = false;
        }

        pending_writes_queue_.clear();
        frame_chunk_buffer_.clear();

        // Reset all per-acquisition counters
        processed_frames_    = 0;
        frames_forwarded_    = 0;
        frames_written_      = 0;
        completed_writes_    = 0;
        pending_writes_count_= 0;
        write_errors_        = 0;
        avg_write_time_us_   = 0;
        last_frame_          = 0;
        highest_frame_written_ = 0;
        current_dataset_capacity_ = 0;
        first_write_recorded_ = false;
        last_error_message_  = "";
        dimension_mismatch_logged_ = false;

        config_.enable_writing_ = true;

        // Sync frame rate from the camera so CSV timestamps are accurate
        CameraController* camera_controller = CameraController::Instance("CameraController_");
        if (camera_controller)
        {
            OdinData::IpcMessage temp_reply;
            if (camera_controller->request_configuration("", temp_reply) &&
                temp_reply.has_param("camera/frames_per_second"))
            {
                frames_per_second_ = temp_reply.get_param<unsigned int>("camera/frames_per_second");
                LOG4CXX_INFO(logger_, "Updated frames_per_second from camera: " << frames_per_second_);
            }
        }

        // Each acquisition gets a fresh, timestamped CSV file
        if (config_.csv_logging_ && !config_.csv_path_.empty())
        {
            csv_logger_.Close(logger_);

            time_t now = time(nullptr);
            char timestamp[32];
            strftime(timestamp, sizeof(timestamp), "%Y%m%d_%H%M%S", localtime(&now));

            std::string csv_filename = streamScopedCsvPath();
            size_t dot_pos = csv_filename.find_last_of('.');
            size_t slash_pos = csv_filename.find_last_of('/');
            // Only treat the dot as an extension if it comes after the final path separator,
            // otherwise a dot in a directory name (e.g. "/perf.d/ts") would have the timestamp
            // spliced into the directory, producing a path that does not exist
            if (dot_pos != std::string::npos &&
                (slash_pos == std::string::npos || dot_pos > slash_pos))
            {
                csv_filename.insert(dot_pos, "_" + std::string(timestamp) + "_" + config_.kvstore_driver_);
            }
            else
            {
                csv_filename += "_" + std::string(timestamp) + "_" + config_.kvstore_driver_ + ".csv";
            }
            csv_path_ = csv_filename;

            csv_logger_.Open(csv_path_, logger_);
            LOG4CXX_INFO(logger_, "New CSV log file: " << csv_path_);
        }

        // Create the new dataset. A configure() message may have cleared the dims, so re-apply the
        // decoder-mode fallback here as well as in the constructor.
        if (config_.height_ == 0)
        {
            config_.height_ = decoder_->get_frame_y_resolution(mode_);
        }
        if (config_.width_ == 0)
        {
            config_.width_ = decoder_->get_frame_x_resolution(mode_);
        }
        config_.bit_depth_ = decoder_->get_frame_bit_depth(mode_);

        std::size_t height = config_.height_;
        std::size_t width  = config_.width_;
        std::size_t bit_depth = config_.bit_depth_;

        // Resolved once here so the log, the dataset spec and any error message all refer to
        // the same path
        const std::string dataset_path = streamScopedDatasetPath();

        LOG4CXX_INFO(logger_, "Creating new dataset in " << datasetDestinationDescription()
            << " with:"
            << " storage_driver=" << config_.storage_driver_
            << " stream=" << config_.stream_id
            << " mode=" << mode_
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
        // Use configured frame count as initial capacity; default to 1000 if unknown
        std::size_t initial_frames = (config_.number_of_frames_ > 0) ? config_.number_of_frames_ : 1000;
        ::nlohmann::json json_spec = FrameProcessor::GetJsonSpec(
            config_.storage_driver_, config_.kvstore_driver_, 
            config_.s3_bucket_, config_.s3_endpoint_,
            data_type_, dataset_path,
            initial_frames, height, width, config_.cache_bytes_limit_
        );
        
        auto store_result = CreateDataset(json_spec);

        if (!store_result.ok()) {
            std::string error_msg = store_result.status().ToString();
            last_error_message_ = TensorstoreErrorHandler::FormatDatasetCreationError(
                error_msg,
                config_.kvstore_driver_,
                config_.s3_endpoint_,
                dataset_path
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
                << initial_frames << " frames. Writing to " << datasetDestinationDescription());
        }
    }

    void TensorstoreCore::forwardFrame(::SuperFrameHeader* frame_buffer, uint64_t frame_number)
    {
        // Pipeline terminus: return buffer directly to the free pool
        if (config_.num_downstream_cores == 0 || downstream_rings_.empty())
        {
            if (clear_frames_ring_ != NULL)
            {
                int ret = rte_ring_enqueue(clear_frames_ring_, frame_buffer);
                if (ret != 0)
                {
                    LOG4CXX_ERROR(logger_, "Failed to return frame " << frame_number
                        << " to clear_frames_ring: " << rte_strerror(-ret));
                }
            }
            else
            {
                LOG4CXX_ERROR(logger_, "clear_frames_ring is NULL, cannot return frame " << frame_number);
            }
            return;
        }

        int ret = rte_ring_enqueue(
            downstream_rings_[frame_number % config_.num_downstream_cores],
            frame_buffer
        );
        if (ret != 0)
        {
            LOG4CXX_ERROR(logger_, "Failed to forward frame " << frame_number
                << " to downstream ring: " << rte_strerror(-ret));
            if (clear_frames_ring_ != NULL)
            {
                rte_ring_enqueue(clear_frames_ring_, frame_buffer);
            }
        }
    }

    void TensorstoreCore::stop(void)
    {
        if (run_lcore_)
        {
            if (tensorstore_initialized_)
            {
                LOG4CXX_INFO(logger_, "Draining " << pending_writes_queue_.size() << " pending writes");
                while (!pending_writes_queue_.empty())
                {
                    pollAndProcessCompletions();
                }
                store_.reset();
                tensorstore_initialized_ = false;
            }
            LOG4CXX_INFO(logger_, "Stopping TensorstoreCore on lcore " << lcore_id_);
            run_lcore_ = false;
        }
    }

    void TensorstoreCore::status(OdinData::IpcMessage& status, const std::string& path)
    {
        // Scope by config key rather than class name so that cores of the same class in different
        // streams report under distinct paths instead of overwriting each other
        std::string core_label = config_.config_key.empty() ? "TensorstoreCore" : config_.config_key;
        std::string status_path = path + "/" + core_label + "_" + std::to_string(proc_idx_) + "/";
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
        status.set_param(ring_status + ring_name_clear_frames(socket_id_, config_.stream_id) + "_count" , rte_ring_count(clear_frames_ring_));
        status.set_param(ring_status + ring_name_clear_frames(socket_id_, config_.stream_id) + "_size" , rte_ring_get_size(clear_frames_ring_));

        status.set_param(status_path + "stream_id", config_.stream_id);
        status.set_param(status_path + "mode", mode_);

        status.set_param(ts_status + "initialized", tensorstore_initialized_);
        status.set_param(ts_status + "storage_path", streamScopedDatasetPath());
        // The backend the path is relative to; under s3 the path is a key prefix in the bucket
        // rather than a local directory, which storage_path alone does not convey.
        status.set_param(ts_status + "kvstore_driver", config_.kvstore_driver_);
        status.set_param(ts_status + "storage_driver", config_.storage_driver_);
        status.set_param(ts_status + "destination", datasetDestinationDescription());
        if (config_.kvstore_driver_ == "s3")
        {
            status.set_param(ts_status + "s3_bucket", config_.s3_bucket_);
            status.set_param(ts_status + "s3_endpoint", config_.s3_endpoint_);
        }
        status.set_param(ts_status + "height", config_.height_);
        status.set_param(ts_status + "width", config_.width_);
        status.set_param(ts_status + "frames_written", frames_written_);
        status.set_param(ts_status + "write_errors", write_errors_);
        status.set_param(ts_status + "avg_write_time_us", avg_write_time_us_);
        status.set_param(ts_status + "pending_writes_queue_size", pending_writes_count_);
        status.set_param(ts_status + "total_completed_writes", completed_writes_);
        status.set_param(ts_status + "enable_writing", config_.enable_writing_);
        status.set_param(ts_status + "last_error", last_error_message_);
    }

    bool TensorstoreCore::connect(void)
    {
        std::string upstream_ring_name = ring_name_str(config_.upstream_core, socket_id_, proc_idx_);
        struct rte_ring* upstream_ring = rte_ring_lookup(upstream_ring_name.c_str());

        if (upstream_ring == NULL)
        {
            LOG4CXX_ERROR(logger_, config_.core_name << " : " << proc_idx_ << " Failed to connect to upstream ring: " << upstream_ring_name);
            return false;
        }
        upstream_ring_ = upstream_ring;

        std::string clear_frames_ring_name = ring_name_clear_frames(socket_id_, config_.stream_id);
        clear_frames_ring_ = rte_ring_lookup(clear_frames_ring_name.c_str());

        if (clear_frames_ring_ == NULL)
        {
            LOG4CXX_ERROR(logger_, config_.core_name << " : " << proc_idx_ << " Failed to connect to clear_frames ring: " << clear_frames_ring_name);
            return false;
        }

        LOG4CXX_INFO(logger_, config_.core_name << " : " << proc_idx_ << " Connected to upstream resources successfully!");
        return true;
    }

    void TensorstoreCore::configure(OdinData::IpcMessage& config)
    {
        LOG4CXX_INFO(logger_, config_.core_name << " : " << proc_idx_ << " Got update config.");

        try
        {
            if (config.has_param("path"))
                config_.path_ = config.get_param<std::string>("path");

            if (config.has_param("storage_driver"))
                config_.storage_driver_ = config.get_param<std::string>("storage_driver");

            if (config.has_param("kvstore_driver"))
                config_.kvstore_driver_ = config.get_param<std::string>("kvstore_driver");

            if (config.has_param("max_concurrent_writes"))
                config_.max_concurrent_writes_ = config.get_param<int>("max_concurrent_writes");

            if (config.has_param("number_of_frames"))
                config_.number_of_frames_ = config.get_param<uint64_t>("number_of_frames");

            // Dataset dims take effect on the next reconfiguration, since resizing an open
            // dataset's frame geometry is not supported
            if (config.has_param("height"))
                config_.height_ = config.get_param<std::size_t>("height");

            if (config.has_param("width"))
                config_.width_ = config.get_param<std::size_t>("width");

            if (config.has_param("frames_per_second"))
                frames_per_second_ = config.get_param<unsigned int>("frames_per_second");

            if (config.has_param("enable_writing"))
            {
                bool enable = config.get_param<bool>("enable_writing");
                if (enable != config_.enable_writing_)
                {
                    config_.enable_writing_ = enable;
                    LOG4CXX_INFO(logger_, config_.core_name << " : " << proc_idx_ << " enable_writing = " << enable);
                    if (!enable && tensorstore_initialized_)
                    {
                        flush_pending_writes = true;
                    }
                }
            }

            if (config.has_param("update_config") && config.get_param<bool>("update_config"))
            {
                handleReconfiguration();
            }
        }
        catch (const std::exception& e)
        {
            LOG4CXX_ERROR(logger_, "Failed to apply configuration: " << e.what());
        }
    }

    std::vector<std::pair<std::string, int>> TensorstoreCore::requestCommands()
    {
        return {
            {"start_writing", DEFAULT_COMMAND_PRIORITY},
            {"stop_writing",  DEFAULT_COMMAND_PRIORITY}
        };
    }

    void TensorstoreCore::execute(const std::string& command, OdinData::IpcMessage& reply)
    {
        if (command == "start_writing")
        {
            start_writing_cmd();
        }
        else if (command == "stop_writing")
        {
            stop_writing_cmd();
        }
        else
        {
            reply.set_nack("TensorstoreCore: unknown command: " + command);
        }
    }

    void TensorstoreCore::start_writing_cmd()
    {
        bool previous_state = config_.enable_writing_;
        config_.enable_writing_ = true;
        if (config_.enable_writing_ != previous_state)
        {
            LOG4CXX_INFO(logger_, config_.core_name << " : " << proc_idx_
                << " start_writing: enable_writing_ = true");
        }
    }

    void TensorstoreCore::stop_writing_cmd()
    {
        bool previous_state = config_.enable_writing_;
        config_.enable_writing_ = false;
        if (config_.enable_writing_ != previous_state)
        {
            LOG4CXX_INFO(logger_, config_.core_name << " : " << proc_idx_
                << " stop_writing: enable_writing_ = false");
            if (tensorstore_initialized_)
            {
                LOG4CXX_DEBUG_LEVEL(2, logger_, "Writing disabled. Flushing "
                    << pending_writes_queue_.size() << " pending writes");
                flush_pending_writes = true;
            }
        }
    }

    void TensorstoreCore::requestConfiguration(OdinData::IpcMessage& reply)
    {
        LOG4CXX_DEBUG(logger_, "Configuration requested for worker core");
    }

    DPDKREGISTER(DpdkWorkerCore, TensorstoreCore, "TensorstoreCore");

} // End of the FrameProcessor namespace