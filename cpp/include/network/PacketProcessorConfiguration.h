#include "ParamContainer.h"
#include "DpdkCoreConfiguration.h"
#include <sstream>

namespace FrameProcessor
{
    namespace Defaults
    {
        const unsigned int default_frame_timeout = 1000;
        const uint32_t default_min_packet_size = 0;   // 0 = no minimum
        const uint32_t default_max_packet_size = 0;   // 0 = no maximum
    }


    class PacketProcessorConfiguration : public OdinData::ParamContainer
    {

        public:

            PacketProcessorConfiguration() :
                ParamContainer(),
                num_cores(0),
                num_downstream_cores(0),
                frame_timeout_(Defaults::default_frame_timeout),
                min_packet_size_(Defaults::default_min_packet_size),
                max_packet_size_(Defaults::default_max_packet_size)
            {
                bind_params();
            }

            void resolve(DpdkCoreConfiguration& core_config_, const std::string& config_key = "packet_processor")
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
                bind_param<unsigned int>(frame_timeout_, "frame_timeout");
                bind_param<uint32_t>(min_packet_size_, "min_packet_size");
                bind_param<uint32_t>(max_packet_size_, "max_packet_size");
            }

            std::string core_name;
            std::string connect;
            std::string upstream_core;
            std::string config_key;
            std::string stream_id;
            std::string decoder_mode;
            unsigned int num_cores;
            unsigned int num_downstream_cores;
            unsigned int frame_timeout_;  //!< Incomplete frame timeout in milliseconds
            uint32_t min_packet_size_;    //!< Minimum packet payload size to process (0 = no limit)
            uint32_t max_packet_size_;    //!< Maximum packet payload size to process (0 = no limit)

            friend class PacketProcessorCore;
    };
}