#include "TensorstoreJsonSpec.h"

namespace FrameProcessor {

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
    uint64_t cache_bytes_limit)
{
    // Builds kvstore specification based on driver type
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
    
    constexpr std::size_t kChunkSize = 1;
    ::nlohmann::json json_spec = {
        {"driver", storage_driver},
        {"kvstore", kvstore_spec},
        {"metadata", {
            {"data_type", data_type},
            {"shape", {frames, height, width}},
            {"chunk_grid", {
                {"name", "regular"},
                {"configuration", {
                    {"chunk_shape", {kChunkSize, height, width}}
                }}
            }}
        }}
    };
    
    if (kvstore_driver == "file") {
        json_spec["context"] = {
            {"data_copy_concurrency", {{"limit", 2048}}},
            {"file_io_concurrency", {{"limit", 2048}}},
            {"cache_pool", {{"total_bytes_limit", cache_bytes_limit * 1024 * 1024}}}
        };
    } else if (kvstore_driver == "s3") {
        json_spec["context"] = {
            {"data_copy_concurrency", {{"limit", 2048}}},
            {"cache_pool", {{"total_bytes_limit", cache_bytes_limit}}}
        };
    }
    
    return json_spec;
}

} // namespace FrameProcessor
