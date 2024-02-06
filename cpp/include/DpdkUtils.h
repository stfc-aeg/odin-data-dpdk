#ifndef INCLUDE_RTEETHER_H_
#define INCLUDE_RTEETHER_H_

#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <rte_ether.h>


#include "DpdkCoreConfiguration.h"
#include "DpdkSharedBuffer.h"

namespace FrameProcessor
{
    std::string mac_addr_str(struct rte_ether_addr& mac_addr);
    std::string ip_addr_str(uint32_t ip_addr);

    unsigned int nearest_power_two(const unsigned int value);

    enum class RingType
    {
        packet_forward, packet_release, frame_ready, frame_processed, frame_built, frame_compressed
    };


    std::string mbuf_pool_name_str(unsigned int socket_idx);
    std::string ring_name_str(std::string UpStreamCore, unsigned int socket_idx, unsigned int core_idx=0);
    std::string ring_name_pkt_release(unsigned int socket_idx);
    std::string ring_name_clear_frames(unsigned int socket_idx);
    std::string shared_mem_name_str(unsigned int socket_idx);

    std::vector<uint16_t> tokenize_port_list(const std::string& port_list_str);
    std::string port_list_str(std::vector<uint16_t>& items);
    uint64_t convert_ms_to_cycles(uint64_t ms);
}

#endif // INCLUDE_RTEETHER_H_