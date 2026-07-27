#include "GeneratedDataSource.h"
#include "PacketProtocolDecoder.h"

namespace FrameProcessor
{

GeneratedDataSource::GeneratedDataSource(
    PacketProtocolDecoder* decoder,
    const std::string& pattern) :
    DataSource(decoder)
{
    if (pattern == "incrementing")
    {
        pattern_ = Pattern::Incrementing;
    }
    else if (pattern == "packetnum")
    {
        pattern_ = Pattern::PacketNum;
    }
    else
    {
        pattern_ = Pattern::Fixed;
    }
}

void GeneratedDataSource::getData(uint16_t* destination)
{
    uint32_t pixels_per_packet =
        frame_pixels_ / packets_per_frame_;

    for (uint32_t pixel = 0;
         pixel < frame_pixels_;
         pixel++)
    {
        uint16_t value = 0;

        switch (pattern_)
        {
            case Pattern::Incrementing:
            {
                uint32_t pos = pixel % 131072;

                value =
                    (pos <= 65535)
                    ? pos
                    : (131071 - pos);

                break;
            }

            case Pattern::PacketNum:
            {
                value = pixel / pixels_per_packet;
                break;
            }

            case Pattern::Fixed:
            {
                value = 84;
                break;
            }
        }

        destination[pixel] = value;
    }
}

}