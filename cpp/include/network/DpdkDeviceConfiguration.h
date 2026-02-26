#ifndef DPDKDEVICECONFIGURATION_H_
#define DPDKDEVICECONFIGURATION_H_

#include "ParamContainer.h"

namespace FrameProcessor
{
    namespace Defaults
    {
        const unsigned int default_mbuf_pool_size = 500000;
        const unsigned int default_mbuf_cache_size = 512;
        const uint32_t default_mtu = 9600;
        const uint16_t default_rx_rings = 1;
        const uint16_t default_rx_num_desc = 16384;
        const uint16_t default_tx_rings = 1;
        const uint16_t default_tx_num_desc = 8192;
    }

    class DpdkDeviceConfiguration : public OdinData::ParamContainer
    {
        public:

            DpdkDeviceConfiguration() :
                ParamContainer(),
                mbuf_pool_size_(Defaults::default_mbuf_pool_size),
                mbuf_cache_size_(Defaults::default_mbuf_cache_size),
                mtu_(Defaults::default_mtu),
                rx_rings_(Defaults::default_rx_rings),
                rx_num_desc_(Defaults::default_rx_num_desc),
                tx_rings_(Defaults::default_tx_rings),
                tx_num_desc_(Defaults::default_tx_num_desc)
            {
                bind_params();
            }

            unsigned int mbuf_pool_size(void) const { return mbuf_pool_size_; }
            unsigned int mbuf_cache_size(void) const { return mbuf_cache_size_; }
            uint32_t mtu(void) const { return mtu_; }
            uint16_t rx_rings(void) const { return rx_rings_; }
            uint16_t rx_num_desc(void) const { return rx_num_desc_; }
            uint16_t tx_rings(void) const { return tx_rings_; }
            uint16_t tx_num_desc(void) const { return tx_num_desc_; }

        private:

            virtual void bind_params(void)
            {
                bind_param<unsigned int>(mbuf_pool_size_, "mbuf_pool_size");
                bind_param<unsigned int>(mbuf_cache_size_, "mbuf_cache_size");
                bind_param<uint32_t>(mtu_, "mtu");
                bind_param<uint16_t>(rx_rings_, "rx_rings");
                bind_param<uint16_t>(rx_num_desc_, "rx_num_desc");
                bind_param<uint16_t>(tx_rings_, "tx_rings");
                bind_param<uint16_t>(tx_num_desc_, "tx_num_desc");
            }

            unsigned int mbuf_pool_size_;   //!< Size of the mbuf pool
            unsigned int mbuf_cache_size_;  //!< Size of the mbuf cache
            uint32_t mtu_;                  //!< Maximum transmission unit
            uint16_t rx_rings_;             //!< Number of RX rings
            uint16_t rx_num_desc_;          //!< Number of RX ring descriptors
            uint16_t tx_rings_;             //!< Number of TX rings
            uint16_t tx_num_desc_;          //!< Number of TX ring descriptors
    };
}

#endif // DPDKDEVICECONFIGURATION_H_
