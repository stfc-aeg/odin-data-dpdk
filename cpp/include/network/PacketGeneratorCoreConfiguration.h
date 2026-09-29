#include "ParamContainer.h"
#include "DpdkCoreConfiguration.h"
#include <sstream>

namespace FrameProcessor
{
    namespace Defaults
    {
        const uint16_t default_destination_port = 1234;
        const uint16_t default_source_port = 2345;
        const std::vector<std::string> default_source_ip_address = {"10.0.0.2"};
        const std::vector<std::string> default_source_mac_address = {"58:a2:e1:c1:fe:e8"};
        const std::vector<std::string> default_destination_ip_address = {"10.0.0.1"};
        const std::vector<std::string> default_destination_mac_address = {"9c:63:c0:db:bb:dc"};
        const std::vector<std::string> default_device_addresses = {"0"};

        //packet_gen_core
        const std::string default_data_source = "generated";
        const std::string default_pattern = "incrementing";
        const std::string default_file_path = "/tmp/acq_1";
        const std::string default_h5_dataset_name = "/dummy";
        const std::string default_round_robin_mode = "frame";

        const uint16_t default_packet_drop = 0;
    }

    class PacketGeneratorConfiguration : public OdinData::ParamContainer
    {
        public:

            PacketGeneratorConfiguration() :
                ParamContainer(),
                destination_port(Defaults::default_destination_port),
                source_port(Defaults::default_source_port),
                source_ip_address(Defaults::default_source_ip_address),
                source_mac_address(Defaults::default_source_mac_address),
                destination_ip_address(Defaults::default_destination_ip_address),
                destination_mac_address(Defaults::default_destination_mac_address),
                device_addresses_(Defaults::default_device_addresses),
                round_robin_mode(Defaults::default_round_robin_mode),
                packet_drop(Defaults::default_packet_drop)
            {
                bind_params();
                data_source_config_json_.SetObject();
            }

            void resolve(DpdkCoreConfiguration& core_config_, const std::string& config_key = "packet_generator")
            {
                const ParamContainer::Value* value_ptr =
                    core_config_.get_worker_core_config(config_key);

                if (value_ptr != nullptr)
                {
                    update(*value_ptr);

                    if (value_ptr->HasMember("dpdk_device"))
                    {
                        dpdk_device_.update((*value_ptr)["dpdk_device"]);
                    }

                    if (value_ptr->HasMember("data_source_config"))
                    {
                        data_source_config_json_.CopyFrom(
                            (*value_ptr)["data_source_config"], data_source_config_json_.GetAllocator());
                    }
                }
            }

            const DpdkDeviceConfiguration& dpdk_device(void) const { return dpdk_device_; }
            DpdkDeviceConfiguration& dpdk_device(void) { return dpdk_device_; }

            const rapidjson::Document& data_source_config(void) const { return data_source_config_json_; }
            rapidjson::Document& data_source_config(void) { return data_source_config_json_; }

        private:

            virtual void bind_params(void)
            {
                bind_param<std::string>(core_name, "core_name");
                bind_param<std::string>(connect, "connect");
                bind_param<std::string>(config_key, "config_key");
                bind_param<std::string>(upstream_core, "upstream_core");
                bind_param<unsigned int>(num_cores, "num_cores");
                bind_param<unsigned int>(num_downstream_cores, "num_downstream_cores");

                bind_param<uint16_t>(destination_port, "destination_port");
                bind_param<uint16_t>(source_port, "source_port");
                bind_vector_param<std::string>(source_ip_address, "source_ip_address");
                bind_vector_param<std::string>(source_mac_address, "source_mac_address");
                bind_vector_param<std::string>(destination_ip_address, "destination_ip_address");
                bind_vector_param<std::string>(destination_mac_address, "destination_mac_address");
                bind_vector_param<std::string>(device_addresses_, "device_addresses");

                bind_param<std::string>(round_robin_mode, "round_robin_mode");
                bind_param<uint16_t>(packet_drop, "packet_drop");
                // data_source / pattern / file_path / dataset_name: no longer bound here
            }

            std::string core_name;
            std::string connect;
            std::string config_key;
            std::string upstream_core;
            unsigned int num_cores;
            unsigned int num_downstream_cores;

            uint16_t destination_port;
            uint16_t source_port;
            std::vector<std::string> source_ip_address;
            std::vector<std::string> source_mac_address;
            std::vector<std::string> destination_ip_address;
            std::vector<std::string> destination_mac_address;
            std::vector<std::string> device_addresses_;
            DpdkDeviceConfiguration dpdk_device_;

            rapidjson::Document data_source_config_json_;

            std::string round_robin_mode;
            uint16_t packet_drop;

            friend class PacketGeneratorCore;
    };
}