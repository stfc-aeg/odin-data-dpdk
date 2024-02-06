#include "DpdkSharedBufferFrame.h"

namespace FrameProcessor {

/**
 * @brief Construct a new DpdkSharedBufferFrame::DpdkSharedBufferFrame object
 * 
 * @param meta_data 
 * @param data_src 
 * @param nbytes 
 * @param frame_processed 
 * @param image_offset 
 */
DpdkSharedBufferFrame::DpdkSharedBufferFrame(const FrameMetaData & meta_data,
                                                 void *data_src,
                                                 size_t nbytes,
                                                 rte_ring *frame_processed,
                                                 const int& image_offset)
    : Frame(meta_data, nbytes, image_offset) {
                                                    
    data_ptr_ = data_src;
    frame_processed_ = frame_processed;
}

/** Copy constructor;
 * implement as shallow copy
 * @param frame
 */
DpdkSharedBufferFrame::DpdkSharedBufferFrame(const DpdkSharedBufferFrame &frame) : Frame(frame)
{
    data_ptr_ = frame.data_ptr_;
    data_size_ = frame.data_size_;
    frame_processed_ = frame.frame_processed_;
}

/**
 * @brief Destroy the Dpdk Shared Buffer Frame:: Dpdk Shared Buffer Frame object
 * 
 */
DpdkSharedBufferFrame::~DpdkSharedBufferFrame () {
    /** Enqueue the memory location back to the starting ring */
    rte_ring_enqueue(frame_processed_, data_ptr_);
}

/**
 * @brief Return the location to the start of frame data
 * 
 * @return void* 
 */
void *DpdkSharedBufferFrame::get_data_ptr() const {
    return data_ptr_;
}

}