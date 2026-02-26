#include "TensorstoreWriter.h"
#include <DebugLevelLogger.h>

namespace FrameProcessor {

// Explicit template instantiations for supported pixel types
template tensorstore::WriteFutures TensorstoreWriter::AsyncWriteFrame<uint8_t>(
    tensorstore::TensorStore<>& store,
    void* raw_data,
    tensorstore::Index height,
    tensorstore::Index width,
    uint64_t frame_number,
    log4cxx::LoggerPtr logger
);

template tensorstore::WriteFutures TensorstoreWriter::AsyncWriteFrame<uint16_t>(
    tensorstore::TensorStore<>& store,
    void* raw_data,
    tensorstore::Index height,
    tensorstore::Index width,
    uint64_t frame_number,
    log4cxx::LoggerPtr logger
);

template tensorstore::WriteFutures TensorstoreWriter::AsyncWriteFrame<uint32_t>(
    tensorstore::TensorStore<>& store,
    void* raw_data,
    tensorstore::Index height,
    tensorstore::Index width,
    uint64_t frame_number,
    log4cxx::LoggerPtr logger
);

template tensorstore::WriteFutures TensorstoreWriter::AsyncWriteFrame<uint64_t>(
    tensorstore::TensorStore<>& store,
    void* raw_data,
    tensorstore::Index height,
    tensorstore::Index width,
    uint64_t frame_number,
    log4cxx::LoggerPtr logger
);

} // namespace FrameProcessor
