/*
 * DpdkSharedBuffer.cpp - a shared buffer class utilising DPDK hugepages memzones.
 *
 * This class implements a shared buffer system for assembling capture data, for instance raw
 * frames, in memory. It abstracts the DPDK memzone implementation in huge pages shared memory.
 *
 * Created on: 05 December 2022
 *     Author: Dominic Banks & Tim Nicholls, STFC Detector Systems Software Group
 */

#include <rte_errno.h>

#include "DpdkSharedBuffer.h"
#include "DpdkUtils.h"
#include <iostream>
namespace FrameProcessor
{
    //! Constructor for the DpdkSharedBuffer class.
    //!
    //! This constructor sets up a shared buffer of a specified total size as a DPDK memzone,
    //! containing the requested number of buffers and mapped to the specified DPDK NUMI socket ID.
    //!
    //! \param[in] mem_size - total memory size in bytes
    //! \param[in] buffer_size - size of each buffer in the memzone
    //! \param[in] socket_id - ID of the DPDK NUMA socket to create the memzone on
    //!
    DpdkSharedBuffer::DpdkSharedBuffer(
        const std::size_t mem_size, const std::size_t buffer_size, const int socket_id
    ):
        mem_size_(mem_size),
        buffer_size_(buffer_size),
        socket_id_(socket_id),
        logger_(Logger::getLogger("FP.DpdkSharedBuffer"))
    {

        // Calculate the number of buffers in the memory zone and check that it is non-zero
        num_buffers_ = mem_size_ / buffer_size_;
        if (!num_buffers_)
        {
            // TODO - buffer size exceeds memory size - should raise an exception
        }

        // Create the memory zone for the shared memory buffer used to assemble frame packets
        name_ = shared_mem_name_str(socket_id_);
        LOG4CXX_DEBUG_LEVEL(2, logger_, "Creating shared memory buffer " << name_
            << " of size " << mem_size_
            << " on socket " << socket_id_
        );
        memzone_ = rte_memzone_reserve(
            name_.c_str(), mem_size_, socket_id_, RTE_MEMZONE_1GB | RTE_MEMZONE_IOVA_CONTIG
        );

        if (memzone_ == NULL)
        {
            memzone_ = rte_memzone_lookup(name_.c_str());
            if (memzone_ == NULL)
            {
                LOG4CXX_ERROR(logger_, "Error creating shared memory buffer " << name_
                        << " on socket " << socket_id_
                        << " : " << rte_strerror(rte_errno)
                );
                // TODO - this is fatal and should raise an exception
            }
        }
    }

    //! Destructor for the DpdkSharedBuffer class.
    //!
    //! This destructor frees the memzone associated with the shared buffer.
    //!
    DpdkSharedBuffer::~DpdkSharedBuffer()
    {
        LOG4CXX_DEBUG_LEVEL(2, logger_, "Freeing shared memory buffer " << name_);
        rte_memzone_free(memzone_);
        memzone_ = NULL;
    }

    //! Get the address of the buffer specified by the given buffer index
    //!
    //! This method gets the address of a specific buffer in the shared buffer memory as specified
    //! by the buffer index.
    //!
    //! \param[in] buffer The index of the buffer to get the address of
    //!
    //! \return void pointer to the start of the specified buffer
    //!
    void* DpdkSharedBuffer::get_buffer_address(const unsigned int buffer) const
    {
        return reinterpret_cast<void *>((char*)memzone_->addr + (buffer * buffer_size_));
    }

    //! Get the number of buffers in the shared buffer
    //!
    //! This method returns the number of buffers in the shared buffer.
    //!
    //! \return The number of buffers in the shared buffer as size_t
    //!
    const std::size_t DpdkSharedBuffer::get_num_buffers(void) const
    {
        return num_buffers_;
    }

    //! Get the size of the buffers in the shared buffer
    //!
    //! This method returns the size of the individual buffers in the shared buffer object.
    //!
    //! \return Size of the buffers in the shared buffer
    //!
    const std::size_t DpdkSharedBuffer::get_buffer_size(void) const
    {
        return buffer_size_;
    }

    //! Get the total memory size of the shared buffer
    //!
    //! This method returns the total memory size of the shared buffer object.
    //!
    //! \return Total memory size of the shared buffer
    //!
    const std::size_t DpdkSharedBuffer::get_mem_size(void) const
    {
        return mem_size_;
    }
}

