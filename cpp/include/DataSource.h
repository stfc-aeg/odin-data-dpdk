#ifndef INCLUDE_DATASOURCE_H_
#define INCLUDE_DATASOURCE_H_

#include <string>
#include "DataBlockFrame.h"
#include "PacketProtocolDecoder.h"

namespace FrameProcessor
{

class DataSource
{
public:

    explicit DataSource(PacketProtocolDecoder* decoder);

    virtual ~DataSource();

    virtual void getData(uint16_t* frame_buffer) = 0;

protected:

    PacketProtocolDecoder* decoder_;

    uint32_t frame_width_;
    uint32_t frame_height_;
    uint32_t frame_pixels_;
    uint32_t packets_per_frame_;
    // uint32_t payload_size_;

};

}

#endif