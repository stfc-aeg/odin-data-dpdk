#include "ParamContainer.h"
#include "DpdkCoreConfiguration.h"
#include <sstream>

namespace FrameProcessor
{

    class FrameBuilderConfiguration : public OdinData::ParamContainer
    {

        public:

            FrameBuilderConfiguration() :
                ParamContainer(),
                num_cores(0),
                num_downstream_cores(0)
            {
                bind_params();
            }

            void resolve(DpdkCoreConfiguration& core_config_, const std::string& config_key = "frame_builder")
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
            }

            std::string core_name;
            std::string connect;
            std::string upstream_core;
            std::string config_key;
            std::string stream_id;
            std::string decoder_mode;
            unsigned int num_cores;
            unsigned int num_downstream_cores;

            friend class FrameBuilderCore;
    };
}