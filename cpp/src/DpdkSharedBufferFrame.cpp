#include "DpdkSharedBufferFrame.h"

namespace FrameProcessor
{
    DpdkSharedBufferFrame::DpdkSharedBufferFrame(
        const FrameMetaData& meta_data,
        void* data_src,
        size_t nbytes,
        rte_ring* frame_processed,
        const int& image_offset
    ) :
        Frame(meta_data, nbytes, image_offset)
    {
        data_ptr_ = data_src;
        frame_processed_ = frame_processed;
    }

    DpdkSharedBufferFrame::DpdkSharedBufferFrame(const DpdkSharedBufferFrame& frame) :
        Frame(frame)
    {
        data_ptr_ = frame.data_ptr_;
        frame_processed_ = frame.frame_processed_;
    }

    DpdkSharedBufferFrame::~DpdkSharedBufferFrame()
    {
        // Return the hugepages buffer to the pool when the last shared_ptr reference drops
        if (frame_processed_ != nullptr)
        {
            rte_ring_enqueue(frame_processed_, data_ptr_);
        }
    }

    void* DpdkSharedBufferFrame::get_data_ptr() const
    {
        return data_ptr_;
    }
}