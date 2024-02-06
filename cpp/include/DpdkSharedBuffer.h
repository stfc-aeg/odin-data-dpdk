/*
 * DpdkSharedBuffer.h - a shared buffer class utilising DPDK hugepages memzones.
 *
 * Created on: 05 December 2022
 *     Author: Dominic Banks & Tim Nicholls, STFC Detector Systems Software Group
 */

#ifndef INCLUDE_DPDKSHAREDBUFFER_H_
#define INCLUDE_DPDKSHAREDBUFFER_H_

#include <rte_memzone.h>

#include <log4cxx/logger.h>
using namespace log4cxx;
using namespace log4cxx::helpers;
#include <DebugLevelLogger.h>

namespace FrameProcessor
{
    class DpdkSharedBuffer
    {
    public:
        DpdkSharedBuffer(
            const std::size_t mem_size, const std::size_t buffer_size,
            const int socket_id=SOCKET_ID_ANY
        );
        ~DpdkSharedBuffer();
        void* get_buffer_address(const unsigned int buffer) const;
        const std::size_t get_num_buffers(void) const;
        const std::size_t get_buffer_size(void) const;
        const std::size_t get_mem_size(void) const;

    private:
        std::size_t mem_size_;    //!< total size of the shared buffer memory zone
        std::size_t buffer_size_; //!< size of each buffer in the shared buffer object
        std::size_t num_buffers_; //!< number of buffers in the shared buffer object
        int socket_id_;           //!< DPDK NUMA socket ID for shared buffer memzone
        std::string name_;        //!< Shared buffer name (used for DPDK lookups)
        const struct rte_memzone *memzone_; //!< Pointer to DPDK memzone structure
        LoggerPtr logger_;        //!< Message logger instance
    };
}

#endif // INCLUDE_DPDKSHAREDBUFFER_H_