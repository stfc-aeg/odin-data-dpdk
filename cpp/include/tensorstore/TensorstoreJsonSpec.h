#ifndef INCLUDE_TENSORSTOREJSONSPEC_H_
#define INCLUDE_TENSORSTOREJSONSPEC_H_

#include <nlohmann/json.hpp>
#include <string>
#include <cstddef>
#include <cstdint>

namespace FrameProcessor {

// Creates a JSON specification for TensorStore configuration
::nlohmann::json GetJsonSpec(
    const std::string& storage_driver,
    const std::string& kvstore_driver,
    const std::string& s3_bucket,
    const std::string& s3_endpoint,
    const std::string& data_type,
    const std::string& path,
    std::size_t frames,
    std::size_t height,
    std::size_t width,
    uint64_t cache_bytes_limit
);

} // namespace FrameProcessor

#endif // INCLUDE_TENSORSTOREJSONSPEC_H_
