#ifndef INCLUDE_DPDKDEVICE_H_
#define INCLUDE_DPDKDEVICE_H_

#include <string>

#include <log4cxx/logger.h>
using namespace log4cxx;
using namespace log4cxx::helpers;
#include <DebugLevelLogger.h>

#include <rte_lcore.h>
#include <rte_debug.h>
#include <rte_errno.h>
#include <rte_ethdev.h>


namespace FrameProcessor
{
    class DpdkDevice
    {
    public:

        DpdkDevice(uint16_t port_id);
        ~DpdkDevice();

        bool start(void);
        bool stop(void);

        inline uint16_t port_id(void) const { return port_id_; }
        inline int socket_id(void) const { return socket_id_; }

    private:

        bool init_mbuf_pool(void);
        bool init_port(void);

        uint16_t port_id_;
        int      socket_id_;

        std::string dev_name_;
        std::string mac_addr_;

        unsigned int mbuf_pool_size_;
        unsigned int mbuf_cache_size_;
        struct rte_mempool* mbuf_pool_;

        uint32_t mtu_;
        uint16_t rx_rings_;
        uint16_t rx_num_desc_;
        uint16_t tx_rings_;
        uint16_t tx_num_desc_;

        LoggerPtr logger_;
    };
}

#endif // INCLUDE_DPDKDEVICE_H_