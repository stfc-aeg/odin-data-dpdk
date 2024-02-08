#include "ParamContainer.h"
#include "DpdkCoreConfiguration.h"
#include <sstream>

namespace FrameProcessor
{

    namespace Defaults
    {
        const std::string default_device_ip = "10.0.0.1";
        const std::vector<uint16_t> default_rx_ports = {1234, 1235};
        const uint16_t default_rx_queue_id = 0;
        const uint16_t default_tx_queue_id = 0;
        const uint16_t default_rx_burst_size = 128;
        const unsigned int default_fwd_ring_size = 32768;
        const unsigned int default_release_ring_size = 32768;
        const unsigned int default_max_packet_tx_retries = 64;
        const unsigned int default_max_packet_queue_retries = 64;
    }

    class PacketRxConfiguration : public OdinData::ParamContainer
    {

        public:

            PacketRxConfiguration() :
                ParamContainer(),
                device_ip_(Defaults::default_device_ip),
                rx_ports_(Defaults::default_rx_ports),
                rx_queue_id_(Defaults::default_rx_queue_id),
                tx_queue_id_(Defaults::default_tx_queue_id),
                rx_burst_size_(Defaults::default_rx_burst_size),
                fwd_ring_size_(Defaults::default_fwd_ring_size),
                release_ring_size_(Defaults::default_release_ring_size),
                max_packet_tx_retries_(Defaults::default_max_packet_tx_retries),
                max_packet_queue_retries_(Defaults::default_max_packet_queue_retries),
                num_processor_cores_(Defaults::default_num_processor_cores)
            {
                bind_params();
            }

            void resolve(DpdkCoreConfiguration& core_config_)
            {
                const ParamContainer::Value* value_ptr =
                    core_config_.get_worker_core_config("packet_rx");

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
                bind_param<unsigned int>(num_cores, "num_cores");
                bind_param<unsigned int>(num_downstream_cores, "num_downstream_cores");
                bind_param<std::string>(device_ip_, "device_ip");
                bind_vector_param<uint16_t>(rx_ports_, "rx_ports");
                bind_param<uint16_t>(rx_queue_id_, "rx_queue_id");
                bind_param<uint16_t>(tx_queue_id_, "tx_queue_id");
                bind_param<uint16_t>(rx_burst_size_, "rx_burst_size");
                bind_param<unsigned int>(fwd_ring_size_, "fwd_ring_size");
                bind_param<unsigned int>(release_ring_size_, "release_ring_size");
                bind_param<unsigned int>(max_packet_tx_retries_, "max_packet_tx_retries");
                bind_param<unsigned int>(max_packet_queue_retries_, "max_packet_queue_retries");

            }

            std::string core_name;
            std::string connect;
            unsigned int num_cores;
            unsigned int num_downstream_cores;
            std::string device_ip_;                 //!< IP address of DPDK NIC device
            std::vector<uint16_t> rx_ports_;        //!< List of ports to receive packets on
            uint16_t rx_queue_id_;                  //!< Packet RX queue UD
            uint16_t tx_queue_id_;                  //!< Packet TX queue ID
            uint16_t rx_burst_size_;                //!< Packet RX burst size
            unsigned int fwd_ring_size_;            //!< Packet forward ring size
            unsigned int release_ring_size_;        //!< Packet release ring size
            unsigned int max_packet_tx_retries_;    //!< Max num of packet RX retries
            unsigned int max_packet_queue_retries_; //!< Max num of packet queue retries

            unsigned int num_processor_cores_;  //!< Number of packet processor cores running

            friend class PacketRxCore;
    };
}