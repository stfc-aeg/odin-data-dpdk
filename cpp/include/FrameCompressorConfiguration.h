#include "ParamContainer.h"
#include "DpdkCoreConfiguration.h"
#include <sstream>

namespace FrameProcessor
{
    class FrameCompressorConfiguration : public OdinData::ParamContainer
    {
        public:

            FrameCompressorConfiguration() :
                ParamContainer(),
                dataset_name_(Defaults::default_dataset_name),
                blosc_clevel_(Defaults::default_blosc_clevel),
                blosc_doshuffle_(Defaults::default_blosc_doshuffle),
                blosc_compcode_(Defaults::default_blosc_compcode),
                blosc_blocksize_(Defaults::default_blosc_blocksize),
                blosc_num_threads_(Defaults::default_blosc_num_threads)
            {
                bind_params();
            }

            void resolve(DpdkCoreConfiguration& core_config_, const std::string& config_key = "frame_compressor")
            {
                const ParamContainer::Value* value_ptr =
                    core_config_.get_worker_core_config(config_key);

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
                bind_param<std::string>(config_key, "config_key");
                bind_param<std::string>(stream_id, "stream_id");
                bind_param<std::string>(decoder_mode, "mode");
                bind_param<unsigned int>(num_cores, "num_cores");
                bind_param<unsigned int>(num_downstream_cores, "num_downstream_cores");

                bind_param<std::string>(dataset_name_, "dataset_name");
                bind_param<unsigned int>(blosc_clevel_, "blosc_clevel");
                bind_param<unsigned int>(blosc_doshuffle_, "blosc_doshuffle");
                bind_param<unsigned int>(blosc_compcode_, "blosc_compcode");
                bind_param<unsigned int>(blosc_blocksize_, "blosc_blocksize");
                bind_param<unsigned int>(blosc_num_threads_, "blosc_num_threads");
            }

            std::string dataset_name_;
            unsigned int blosc_clevel_;
            unsigned int blosc_doshuffle_;
            unsigned int blosc_compcode_;
            unsigned int blosc_blocksize_;
            unsigned int blosc_num_threads_;

            std::string core_name;
            std::string connect;
            std::string upstream_core;
            std::string config_key;
            std::string stream_id;
            std::string decoder_mode;
            unsigned int num_cores;
            unsigned int num_downstream_cores;

            friend class FrameCompressorCore;
    };
}