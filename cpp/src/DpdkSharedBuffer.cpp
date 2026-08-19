#include <rte_errno.h>

#include "DpdkSharedBuffer.h"
#include "DpdkUtils.h"

namespace FrameProcessor
{
    DpdkSharedBuffer::DpdkSharedBuffer(
        const std::size_t mem_size, const std::size_t buffer_size,
        const int socket_id, const std::string& stream_id
    ):
        mem_size_(mem_size),
        buffer_size_(buffer_size),
        socket_id_(socket_id),
        logger_(Logger::getLogger("FP.DpdkSharedBuffer"))
    {

        num_buffers_ = mem_size_ / buffer_size_;
        if (num_buffers_ == 0)
        {
            throw std::runtime_error("DpdkSharedBuffer: buffer_size (" + std::to_string(buffer_size_)
                + ") exceeds mem_size (" + std::to_string(mem_size_) + "); zero buffers would be allocated");
        }

        name_ = shared_mem_name_str(socket_id_, stream_id);
        LOG4CXX_INFO(logger_, "Creating shared memory buffer " << name_
            << " size " << mem_size_
            << " socket " << socket_id_
            << " num_buffers " << num_buffers_
            << " buffer_size " << buffer_size_
        );

        // Look up an existing memzone first (e.g. in a secondary DPDK process)
        memzone_ = rte_memzone_lookup(name_.c_str());
        if (memzone_ != NULL)
        {
            LOG4CXX_INFO(logger_, "Found existing shared memory buffer " << name_);
            return;
        }

        memzone_ = rte_memzone_reserve(
            name_.c_str(), mem_size_, socket_id_, RTE_MEMZONE_1GB
        );

        if (memzone_ == NULL)
        {
            LOG4CXX_ERROR(logger_, "Error creating shared memory buffer " << name_
                        << " on socket " << socket_id_
                        << " : " << rte_strerror(rte_errno)
                        << " : " << rte_errno
                );
            throw std::runtime_error("Failed to create or find shared memory buffer");
        }
    }

    DpdkSharedBuffer::~DpdkSharedBuffer()
    {
        LOG4CXX_DEBUG_LEVEL(2, logger_, "Freeing shared memory buffer " << name_);
        rte_memzone_free(memzone_);
        memzone_ = NULL;
    }

    void* DpdkSharedBuffer::get_buffer_address(const unsigned int buffer) const
    {
        return reinterpret_cast<void *>((char*)memzone_->addr + (buffer * buffer_size_));
    }

    const std::size_t DpdkSharedBuffer::get_num_buffers(void) const { return num_buffers_; }
    const std::size_t DpdkSharedBuffer::get_buffer_size(void) const { return buffer_size_; }
    const std::size_t DpdkSharedBuffer::get_mem_size(void) const { return mem_size_; }
}

