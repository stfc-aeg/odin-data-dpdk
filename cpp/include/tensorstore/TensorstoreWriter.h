#ifndef INCLUDE_TENSORSTOREWRITER_H_
#define INCLUDE_TENSORSTOREWRITER_H_

#include <tensorstore/tensorstore.h>
#include <tensorstore/array.h>
#include <tensorstore/index.h>
#include <tensorstore/strided_layout.h>
#include <tensorstore/util/future.h>
#include <tensorstore/index_space/dim_expression.h>
#include <log4cxx/logger.h>
#include <DebugLevelLogger.h>
#include <array>
#include <cstdint>

namespace FrameProcessor {

// Class for writing frame data to tensorstore
class TensorstoreWriter {
public:
    // Asynchronously writes a single frame to tensorstore
    //
    // This template function handles asynchronous writing of a 2D frame:
    // 1. Casts raw memory to the correct data type
    // 2. Creates a 2D view of the image data (zero-copy)
    // 3. Selects the target 2D slice in the store
    // 4. Starts the write operation
    // 5. Returns a Future that tracks the write completion

    template <typename T>
    static tensorstore::WriteFutures AsyncWriteFrame(
        tensorstore::TensorStore<>& store,
        void* raw_data,
        tensorstore::Index height,
        tensorstore::Index width,
        uint64_t frame_number,
        log4cxx::LoggerPtr logger
    );

    // Asynchronously writes multiple frames (chunk) to tensorstore
    //
    // This template function handles asynchronous writing of multiple 2D frames within a "super frame":
    // 1. Casts raw memory to the correct data type
    // 2. Creates a 3D view of the image data (zero-copy) with shape [num_frames, height, width]
    // 3. Selects the target 3D slice in the store
    // 4. Starts the write operation
    // 5. Returns a Future that tracks the write completion

    template <typename T>
    static tensorstore::WriteFutures AsyncWriteFrameChunk(
        tensorstore::TensorStore<>& store,
        void* raw_data,
        tensorstore::Index num_frames,
        tensorstore::Index height,
        tensorstore::Index width,
        uint64_t starting_frame_number,
        log4cxx::LoggerPtr logger
    );
};


template <typename T>
tensorstore::WriteFutures TensorstoreWriter::AsyncWriteFrame(
    tensorstore::TensorStore<>& store,
    void* raw_data,
    tensorstore::Index height,
    tensorstore::Index width,
    uint64_t frame_number,
    log4cxx::LoggerPtr logger)
{
    // 1. Cast raw memory to the correct data type
    T* data_as_T = reinterpret_cast<T*>(raw_data);

    // 2. Describe the 2D image's memory layout
    std::array<tensorstore::Index, 2> shape = {height, width};
    std::array<tensorstore::Index, 2> byte_strides = {
        width * static_cast<tensorstore::Index>(sizeof(T)),  //row
        static_cast<tensorstore::Index>(sizeof(T))            //column
    };
    tensorstore::StridedLayout<2> layout(shape, byte_strides);

    // 3. Create a view of the data (zero-copy)
    tensorstore::ArrayView<T, 2> two_dim_view(data_as_T, layout);

    // 4. Convert the view to a shared array for safe writing
    auto array2 = tensorstore::StaticRankCast<2>(tensorstore::UnownedToShared(two_dim_view));

    // 5. Select the target 2D slice
    tensorstore::Index target_frame_index = static_cast<tensorstore::Index>(frame_number);
    auto target = store | tensorstore::Dims(0).IndexSlice(target_frame_index);

    // 6. Write the 2D image data to the target slice
    LOG4CXX_DEBUG_LEVEL(2, logger, "Calling write for frame " << frame_number);
    auto write_future = tensorstore::Write(array2, target);

    // 7. Return the future immediately
    return write_future;
}

template <typename T>
tensorstore::WriteFutures TensorstoreWriter::AsyncWriteFrameChunk(
    tensorstore::TensorStore<>& store,
    void* raw_data,
    tensorstore::Index num_frames,
    tensorstore::Index height,
    tensorstore::Index width,
    uint64_t starting_frame_number,
    log4cxx::LoggerPtr logger)
{
    // 1. Cast raw memory to the correct data type
    T* data_as_T = reinterpret_cast<T*>(raw_data);

    // 2. Describe the 3D chunk's memory layout
    std::array<tensorstore::Index, 3> shape = {num_frames, height, width};
    std::array<tensorstore::Index, 3> byte_strides = {
        height * width * static_cast<tensorstore::Index>(sizeof(T)), // frame 
        width * static_cast<tensorstore::Index>(sizeof(T)),   // row 
        static_cast<tensorstore::Index>(sizeof(T))  // column 
    };
    tensorstore::StridedLayout<3> layout(shape, byte_strides);

    // 3. Create a view of the data (zero-copy)
    tensorstore::ArrayView<T, 3> three_dim_view(data_as_T, layout);

    // 4. Convert the view to a shared array for safe writing
    auto array3 = tensorstore::StaticRankCast<3>(tensorstore::UnownedToShared(three_dim_view));

    // 5. Select the target 3D slice in the store 
    tensorstore::Index start_idx = static_cast<tensorstore::Index>(starting_frame_number);
    tensorstore::Index end_idx = start_idx + num_frames;
    auto target = store | tensorstore::Dims(0).HalfOpenInterval(start_idx, end_idx);

    // 6. Write the 3D chunk data to the target slice
    LOG4CXX_DEBUG_LEVEL(2, logger, "Calling write for " << num_frames 
        << " frames starting at frame " << starting_frame_number);
    auto write_future = tensorstore::Write(array3, target);

    // 7. Return the future immediately
    return write_future;
}

} // namespace FrameProcessor

#endif // INCLUDE_TENSORSTOREWRITER_H_
