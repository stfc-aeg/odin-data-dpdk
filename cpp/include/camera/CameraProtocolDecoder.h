#ifndef INCLUDE_CAMERA_PROTOCOL_DECODER_H_
#define INCLUDE_CAMERA_PROTOCOL_DECODER_H_

#include <string>
#include "ProtocolDecoder.h"

class CameraProtocolDecoder : public ProtocolDecoder
{
public:

    CameraProtocolDecoder(
        const std::size_t payload_size = 0,  unsigned int frames_per_superframe = 1
    ) :
        ProtocolDecoder(payload_size, frames_per_superframe)
    {

    }

    virtual ~CameraProtocolDecoder() { };

    virtual const std::size_t get_frame_data_size(const std::string& mode = "") const override
    {
        return payload_size_;
    }



protected:

};
#endif // INCLUDE_CAMERA_PROTOCOL_DECODER_H_