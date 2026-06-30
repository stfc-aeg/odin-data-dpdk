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
        // const uint64_t default_number_of_frames = 2000;
        const std::string default_test_pattern_mode = "numerical-incremental";
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
                // device_addresses(Defaults::default_device_addresses),

                device_addresses_(Defaults::default_device_addresses),
                
                // number_of_frames(Defaults::default_number_of_frames),
                test_pattern_mode(Defaults::default_test_pattern_mode)

            {
                bind_params();
            }

            void resolve(DpdkCoreConfiguration& core_config_)
            {
                const ParamContainer::Value* value_ptr =
                    core_config_.get_worker_core_config("packet_generator");

                if (value_ptr != nullptr)
                {
                    update(*value_ptr);

                    // Resolve the dpdk_device subsection if present
                    if (value_ptr->HasMember("dpdk_device"))
                    {
                        dpdk_device_.update((*value_ptr)["dpdk_device"]);
                    }
                }        
            }

            const DpdkDeviceConfiguration& dpdk_device(void) const { return dpdk_device_; }
            DpdkDeviceConfiguration& dpdk_device(void) { return dpdk_device_; }

        private:

            virtual void bind_params(void)
            {
                bind_param<std::string>(core_name, "core_name");
                bind_param<std::string>(connect, "connect");
                bind_param<std::string>(upstream_core, "upstream_core");
                bind_param<unsigned int>(num_cores, "num_cores");
                bind_param<unsigned int>(num_downstream_cores, "num_downstream_cores");


                bind_param<uint16_t>(destination_port, "destination_port");
                bind_param<uint16_t>(source_port, "source_port");
                bind_vector_param<std::string>(source_ip_address, "source_ip_address");
                bind_vector_param<std::string>(source_mac_address, "source_mac_address");
                bind_vector_param<std::string>(destination_ip_address, "destination_ip_address");
                bind_vector_param<std::string>(destination_mac_address, "destination_mac_address");
                // bind_param<std::vector<std::string>>(device_addresses, "device_addresses");
                bind_vector_param<std::string>(device_addresses_, "device_addresses");

                // bind_param<uint64_t>(number_of_frames, "number_of_frames");
                bind_param<std::string>(test_pattern_mode, "test_pattern_mode");
            }

            std::string core_name;
            std::string connect;
            std::string upstream_core;
            unsigned int num_cores;
            unsigned int num_downstream_cores;

            //tx_worker_core
            uint16_t destination_port;
            uint16_t source_port;
            std::vector<std::string> source_ip_address;
            std::vector<std::string> source_mac_address;
            std::vector<std::string> destination_ip_address;
            std::vector<std::string> destination_mac_address;
            std::vector<std::string> device_addresses_;
            DpdkDeviceConfiguration dpdk_device_;

            //packet_gen_core
            // uint64_t number_of_frames;
            std::string test_pattern_mode;


            friend class PacketGeneratorCore;
    };
}