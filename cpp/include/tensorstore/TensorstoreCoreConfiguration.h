#ifndef INCLUDE_TENSORSTORECORECONFIGURATION_H_
#define INCLUDE_TENSORSTORECORECONFIGURATION_H_

#include "ParamContainer.h"
#include "DpdkCoreConfiguration.h"
#include <sstream>

namespace FrameProcessor
{
    namespace Defaults
    {
        const std::string kDefaultDatasetPath = "/tmp";
        const bool kDefaultEnableWriting = false;
        const bool kDefaultCsvLogging = false;
        const std::string kDefaultCsvPath = "/tmp";
        const std::string kDefaultKvstoreDriver = "file";
        const std::string kDefaultS3Bucket = "";
        const std::string kDefaultS3Endpoint = "";
        const uint64_t kDefaultCacheBytesLimit = 10737418240ULL;
        const unsigned int kDefaultNumberOfFrames = 1000;
    }
    class TensorstoreCoreConfiguration : public OdinData::ParamContainer
    {
        public:
            TensorstoreCoreConfiguration() :
                ParamContainer(),
                path_(Defaults::kDefaultDatasetPath),
                enable_writing_(Defaults::kDefaultEnableWriting),
                csv_logging_(Defaults::kDefaultCsvLogging),
                csv_path_(Defaults::kDefaultCsvPath),
                kvstore_driver_(Defaults::kDefaultKvstoreDriver),
                s3_bucket_(Defaults::kDefaultS3Bucket),
                s3_endpoint_(Defaults::kDefaultS3Endpoint),
                cache_bytes_limit_(Defaults::kDefaultCacheBytesLimit),
                number_of_frames_(Defaults::kDefaultNumberOfFrames)
            {
                bind_params();
            }
            
            void resolve(DpdkCoreConfiguration &core_config_)
            {
                const ParamContainer::Value* value_ptr =
                    core_config_.get_worker_core_config("tensorstore");

                if (value_ptr != nullptr)
                {
                    update(*value_ptr);
                    
                }
            }
            
            
        private:
            virtual void bind_params(void)
            {
                bind_param<std::string>(core_name, "core_name");
                bind_param<std::string>(connect, "connect");
                bind_param<std::string>(upstream_core, "upstream_core");
                bind_param<unsigned int>(num_cores, "num_cores");
                bind_param<unsigned int>(num_downstream_cores, "num_downstream_cores");
                bind_param<std::string>(storage_path_, "storage_path");
                bind_param<unsigned int>(number_of_frames_, "number_of_frames");
                bind_param<unsigned int>(frame_size_, "frame_size");
                bind_param<unsigned int>(chunk_size_, "chunk_size");
                bind_param<uint64_t>(cache_bytes_limit_, "cache_bytes_limit");
                bind_param<unsigned int>(data_copy_concurrency_, "data_copy_concurrency");
                bind_param<bool>(delete_existing_, "delete_existing");
                bind_param<std::size_t>(height_, "height");
                bind_param<std::size_t>(width_, "width");
                bind_param<std::size_t>(bit_depth_, "bit_depth");
                bind_param<std::string>(path_, "path");
                bind_param<std::string>(storage_driver_, "storage_driver");
                bind_param<std::string>(kvstore_driver_, "kvstore_driver");
                bind_param<std::string>(s3_bucket_, "s3_bucket");
                bind_param<std::string>(s3_endpoint_, "s3_endpoint");
                bind_param<int>(max_concurrent_writes_, "max_concurrent_writes");
                bind_param<bool>(enable_writing_, "enable_writing");
                bind_param<bool>(csv_logging_, "csv_logging");
                bind_param<std::string>(csv_path_, "csv_path");
            }
            
            std::string core_name;
            std::string connect;
            std::string upstream_core;
            unsigned int num_cores;
            unsigned int num_downstream_cores;
            
            // TensorStore specific config
            std::string storage_path_;
            unsigned int number_of_frames_;
            unsigned int frame_size_;
            unsigned int chunk_size_;
            uint64_t cache_bytes_limit_;
            unsigned int data_copy_concurrency_;
            bool delete_existing_;
            std::size_t height_;
            std::size_t width_;
            std::size_t bit_depth_;
            std::string path_;
            std::string storage_driver_;
            std::string kvstore_driver_;
            std::string s3_bucket_;
            std::string s3_endpoint_;
            int max_concurrent_writes_;
            bool enable_writing_;
            bool csv_logging_;
            std::string csv_path_;
            friend class TensorstoreCore;
    };
}

#endif // INCLUDE_TENSORSTORECORECONFIGURATION_H_