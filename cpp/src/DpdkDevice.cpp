#include "DpdkDevice.h"
#include "DpdkUtils.h"

namespace FrameProcessor
{
    DpdkDevice::DpdkDevice(uint16_t port_id) :
        port_id_(port_id),
        mbuf_pool_size_(1048575), // TODO - parameterise these - pass in as input config?
        mbuf_cache_size_(500),
        mtu_(9600), 
        rx_rings_(1),
        rx_num_desc_(8192),
        tx_rings_(1),
        tx_num_desc_(8192),
        logger_(Logger::getLogger("FP.DpdkDevice"))
    {

        int rc;

        // Get the NUMA socket ID for this device port is connected to
        socket_id_ = rte_eth_dev_socket_id(port_id_);

        // Get the PCI device name
        char dev_name[RTE_DEV_NAME_MAX_LEN];
        rc = rte_eth_dev_get_name_by_port(port_id_, dev_name);
        if (rc != 0)
        {
            LOG4CXX_ERROR(logger_, "Error getting PCI device name for device on port " << port_id_
                << " : " << rte_strerror(rc)
            );
            snprintf(dev_name, sizeof(dev_name), "unknown");
        }
        dev_name_ = std::string(dev_name);

        // Get the device MAC address
        struct rte_ether_addr dev_eth_addr;
        char mac_str[19];
        rc = rte_eth_macaddr_get(port_id_, &dev_eth_addr);
        if (rc != 0)
        {
            LOG4CXX_ERROR(logger_, "Error getting MAC address for device on port " << port_id_
                << " : " << rte_strerror(rc)
            );
            snprintf(dev_name, sizeof(mac_str), "unknown");
        }
        else
        {
            snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x",
                dev_eth_addr.addr_bytes[0], dev_eth_addr.addr_bytes[1], dev_eth_addr.addr_bytes[2],
                dev_eth_addr.addr_bytes[3], dev_eth_addr.addr_bytes[4], dev_eth_addr.addr_bytes[5]
            );
        }
        mac_addr_ = std::string(mac_str);

        // Report the identity of the device
        LOG4CXX_INFO(logger_, "Found ethernet device: " << port_id_
            << " PCI device name: " << dev_name_
            << " MAC: " << mac_addr_
            << " socket: " << socket_id_
        );

        if (!init_mbuf_pool())
        {
            // TODO - this is fatal so should raise an exception
            return;
        }

        if (!init_port())
        {
            // TODO - this is fatal so should raise an exception
            return;
        }

    }

    DpdkDevice::~DpdkDevice()
    {
        LOG4CXX_DEBUG_LEVEL(2, logger_, "DpdkDevice destructor");
        stop();
    }

    bool DpdkDevice::init_mbuf_pool(void)
    {

        std::string mbuf_pool_name = mbuf_pool_name_str(socket_id_);

        LOG4CXX_DEBUG_LEVEL(2, logger_, "Creating packet mbuf pool " << mbuf_pool_name
            << " for device on port " << port_id_
            << " socket " << socket_id_
        );
        mbuf_pool_ = rte_pktmbuf_pool_create(
            mbuf_pool_name.c_str(), mbuf_pool_size_, mbuf_cache_size_,
            RTE_MBUF_PRIV_ALIGN, mtu_, socket_id_
        );

        if (mbuf_pool_ == NULL)
        {
            mbuf_pool_ = rte_mempool_lookup(mbuf_pool_name.c_str());
            if (mbuf_pool_ == NULL)
            {
                LOG4CXX_ERROR(logger_, "Error creating mbuf pool for device on port " << port_id_
                    << " socket " << socket_id_
                    << " : " << rte_strerror(rte_errno)
                );
                return false;
            }
        }

        return true;
    }

    bool DpdkDevice::init_port(void)
    {
        int rc = 0;

        // Get the device info
        struct rte_eth_dev_info dev_info;
        rc = rte_eth_dev_info_get(port_id_, &dev_info);
        if (rc != 0)
        {
            LOG4CXX_ERROR(logger_, "Error getting ethernet device info for port " << port_id_
                 << " : " << rte_strerror(rc)
            );

            // TODO - this is fatal so should raise an exception
        }

        // Intialise default port configuration
        struct rte_eth_conf port_conf = {
            .rxmode = {
                .mtu = mtu_,
            }
        };

        // Set offload capability for TX path if available
        if (dev_info.tx_offload_capa & DEV_TX_OFFLOAD_MBUF_FAST_FREE)
        {
            LOG4CXX_DEBUG_LEVEL(2, logger_,
                "Enabling TX offload for device on port " << port_id_
            );
            port_conf.txmode.offloads |= DEV_TX_OFFLOAD_MBUF_FAST_FREE;
        }

        // Enable RX offload scatter to support reception of jumbo frames
        if (dev_info.rx_offload_capa & DEV_RX_OFFLOAD_SCATTER);
        {
            LOG4CXX_DEBUG_LEVEL(2, logger_,
                "Enabling RX offload scatter for device on port " << port_id_
            );
            port_conf.rxmode.offloads |= DEV_RX_OFFLOAD_SCATTER;
        }

        // Apply the configuration to the device
        rc = rte_eth_dev_configure(port_id_, rx_rings_, tx_rings_, &port_conf);
        if (rc != 0)
        {
            LOG4CXX_ERROR(logger_, "Error setting device configuration for port " << port_id_
                 << " : " << rte_strerror(rc)
            );
            return false;
        }

        // Adjust the number of RX and TX ring descriptors on the device
        rc = rte_eth_dev_adjust_nb_rx_tx_desc(port_id_, &rx_num_desc_, &tx_num_desc_);
        if (rc != 0)
        {
            LOG4CXX_ERROR(logger_, "Error adjusting RX/TX ring descriptors for port " << port_id_
                 << " : " << rte_strerror(rc)
            );
            return false;
        }

        // Set up a RX queue for the device - TODO - confirm queue_id and why rxconf is null
        uint16_t rx_queue_id = 0;
        rc = rte_eth_rx_queue_setup(
            port_id_, rx_queue_id, rx_num_desc_, socket_id_, NULL, mbuf_pool_
        );
        if (rc != 0)
        {
            LOG4CXX_ERROR(logger_, "Error setting up RX queue for device on port " << port_id_
                << " : " << rte_strerror(rc)
            );
            return false;
        }

        // Set up a TX queue for the device
        uint16_t tx_queue_id = 0;
        struct rte_eth_txconf txconf = dev_info.default_txconf;
        txconf.offloads = port_conf.txmode.offloads;

        rc = rte_eth_tx_queue_setup(port_id_, tx_queue_id, tx_num_desc_, socket_id_, &txconf);
        if (rc != 0)
        {
            LOG4CXX_ERROR(logger_, "Error setting up TX queue for device on port " << port_id_
                << " : " << rte_strerror(rc)
            );
            return false;
        }

        return true;
    }

    bool DpdkDevice::start(void)
    {
        int rc;

        LOG4CXX_INFO(logger_, "Starting ethernet device on port " << port_id_);

        // Start the device
        rc = rte_eth_dev_start(port_id_);
        if (rc != 0)
        {
            LOG4CXX_ERROR(logger_, "Error starting the device on port " << port_id_
                << " : " << rte_strerror(rc)
            );
            return false;
        }

        // Enable promiscuous mode for the device
        rc = rte_eth_promiscuous_enable(port_id_);
        if (rc != 0)
        {
            LOG4CXX_ERROR(logger_, "Error enabling promiscuous mode for device on port " << port_id_
                << " : " << rte_strerror(rc)
            );
            return false;
        }

        return true;
    }

    bool DpdkDevice::stop(void)
    {
        int rc;

        LOG4CXX_INFO(logger_, "Stopping ethernet device on port " << port_id_);
        rc = rte_eth_dev_stop(port_id_);
        if (rc != 0)
        {
            LOG4CXX_ERROR(logger_, "Error stopping the device on port " << port_id_
                << " : " << rte_strerror(rc)
            );
            return false;
        }
        return true;
    }
}