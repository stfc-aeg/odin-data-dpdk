#include "DpdkUtils.h"

#include <vector>
#include <iterator>
#include <sstream>

#include <boost/format.hpp>

namespace FrameProcessor
{
    std::string mac_addr_str(struct rte_ether_addr& mac_addr)
    {
        std::stringstream ss;

        ss  << boost::format("%02x:") % (unsigned int)(mac_addr.addr_bytes[0])
            << boost::format("%02x:") % (unsigned int)(mac_addr.addr_bytes[1])
            << boost::format("%02x:") % (unsigned int)(mac_addr.addr_bytes[2])
            << boost::format("%02x:") % (unsigned int)(mac_addr.addr_bytes[3])
            << boost::format("%02x:") % (unsigned int)(mac_addr.addr_bytes[4])
            << boost::format("%02x" ) % (unsigned int)(mac_addr.addr_bytes[5]);

        return ss.str();
    }

    std::string ip_addr_str(uint32_t ip_addr)
    {
        std::stringstream ss;
        uint8_t* addr_bytes = (uint8_t*)&ip_addr;

        ss <<  boost::format("%u.") % (unsigned int)(addr_bytes[0]);
        ss <<  boost::format("%u.") % (unsigned int)(addr_bytes[1]);
        ss <<  boost::format("%u.") % (unsigned int)(addr_bytes[2]);
        ss <<  boost::format("%u" ) % (unsigned int)(addr_bytes[3]);

        return ss.str();
    }

    unsigned int nearest_power_two(const unsigned int value)
    {
        int nearest_power = 2;

        while (nearest_power < value)
        {
            nearest_power *= 2;
        }

        return nearest_power;
    }

    std::string mbuf_pool_name_str(unsigned int socket_idx)
    {
        std::stringstream ss;

        ss << boost::format("mbuf_pool_%02u") % socket_idx;

        return ss.str();
    }

    std::string ring_name_str(std::string UpStreamCore, unsigned int socket_idx, unsigned int core_idx)
    {
        std::stringstream ss;

        ss << boost::format("%s_%02u_%u") % UpStreamCore % core_idx % socket_idx;

        return ss.str();
    }

    std::string ring_name_pkt_release(unsigned int socket_idx)
    {
        std::stringstream ss;

        ss << boost::format("packet_release_%u") % socket_idx;

        return ss.str();
    }

    std::string ring_name_clear_frames(unsigned int socket_idx)
    {
        std::stringstream ss;

        ss << boost::format("clear_frames_%u") % socket_idx;

        return ss.str();
    }

    std::string shared_mem_name_str(unsigned int socket_idx)
    {
        std::stringstream ss;

        ss << boost::format("smb_%02u") % socket_idx;

        return ss.str();
    }

    std::vector<uint16_t> tokenize_port_list(const std::string& port_list_str)
    {

        std::vector<uint16_t> port_list;

        const std::string delimiter(",");
        std::size_t start = 0, end = 0;

        while (end != std::string::npos)
        {
            end = port_list_str.find(delimiter, start);
            const char* port_str = port_list_str.substr(
                start, (end == std::string::npos) ? std::string::npos : end - start
            ).c_str();
            start = (
                (end > (std::string::npos - delimiter.size())) ?
                    std::string::npos : end + delimiter.size()
            );

            uint16_t port = static_cast<uint16_t>(strtol(port_str, NULL, 0));
            if (port != 0)
            {
                port_list.push_back(port);
            }
        }

        return port_list;
    }

    std::string port_list_str(std::vector<uint16_t>& items)
    {
        std::ostringstream ss;
        std::copy(items.begin(), items.end() - 1, std::ostream_iterator<uint16_t>(ss, ", "));
        ss << items.back();
        return ss.str();
    }

    uint64_t convert_ms_to_cycles(uint64_t ms) {
        return rte_get_tsc_hz() * ms / 1000;
    }
}