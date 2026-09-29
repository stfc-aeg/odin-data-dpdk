#ifndef INCLUDE_DATASOURCE_H_
#define INCLUDE_DATASOURCE_H_

#include <string>
#include <log4cxx/logger.h>
using namespace log4cxx;
using namespace log4cxx::helpers;
#include <DebugLevelLogger.h>

#include "DataBlockFrame.h"
#include "PacketProtocolDecoder.h"

namespace FrameProcessor
{

class DataSource
{
public:

    explicit DataSource(PacketProtocolDecoder* decoder, FrameProcessor::DataType data_type);

    virtual ~DataSource();

    virtual void getData(void* destination) = 0;

protected:

    LoggerPtr logger_;

    PacketProtocolDecoder* decoder_;
    FrameProcessor::DataType data_type_;

    uint32_t frame_width_;
    uint32_t frame_height_;
    uint32_t frame_pixels_;
    uint32_t packets_per_frame_;
};

}

#endif