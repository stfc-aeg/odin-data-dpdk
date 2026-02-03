#ifndef INCLUDE_TENSORSTORECORECONFIGURATION_H_
#define INCLUDE_TENSORSTORECORECONFIGURATION_H_

#include "ParamContainer.h"
#include "DpdkCoreConfiguration.h"
#include <sstream>

namespace FrameProcessor
{
    namespace Defaults
    {
        const std::string default_dataset_path = "/tmp";
        const bool default_enable_writing = false;
        const bool default_csv_logging = false;
        const std::string default_csv_path = "/tmp";
        const std::string default_kvstore_driver = "file";
        const std::string default_s3_bucket = "";
        const std::string default_s3_endpoint = "";
    }
    class TensorstoreCoreConfiguration : public OdinData::ParamContainer
    {
        public:
            TensorstoreCoreConfiguration() :
                ParamContainer(),
                path_(Defaults::default_dataset_path),
                enable_writing_(Defaults::default_enable_writing),
                csv_logging_(Defaults::default_csv_logging),
                csv_path_(Defaults::default_csv_path),
                kvstore_driver_(Defaults::default_kvstore_driver),
                s3_bucket_(Defaults::default_s3_bucket),
                s3_endpoint_(Defaults::default_s3_endpoint)
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