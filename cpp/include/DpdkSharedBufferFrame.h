#ifndef INCLUDE_DPDKSHAREDBUFFERFRAME_H_
#define INCLUDE_DPDKSHAREDBUFFERFRAME_H_

#include "rte_ring.h"
#include "Frame.h"
namespace FrameProcessor {

class DpdkSharedBufferFrame : public Frame {

    public:
    /**
     * @brief Construct a new Dpdk Shared Buffer Frame object
     * 
     * @param meta_data 
     * @param data_src 
     * @param nbytes 
     * @param frame_processed 
     * @param image_offset 
     */
    DpdkSharedBufferFrame(const FrameMetaData & meta_data,
                            void *data_src,
                            size_t nbytes,
                            rte_ring *frame_processed,
                            const int& image_offset = 0);

    /**
     * @brief Shallow-copy copy 
     * 
     * @param DpdkSharedBufferFrame 
     */
    DpdkSharedBufferFrame(const DpdkSharedBufferFrame &frame);

    /** Destructor */
    ~DpdkSharedBufferFrame();

    /** Return the address of start of frame data*/
    virtual void *get_data_ptr() const;

  private:

  void *image_ptr_;
  void *data_ptr_;

  rte_ring *frame_processed_;  

};

}

#endif