#ifndef INCLUDE_PACKET_PROTOCOL_DECODER_H_
#define INCLUDE_PACKET_PROTOCOL_DECODER_H_

#include "ProtocolDecoder.h"

struct PacketHeader { };

class PacketProtocolDecoder : public ProtocolDecoder
{
public:

    PacketProtocolDecoder(
        const std::size_t packets_per_frame, const std::size_t payload_size,
        const unsigned int frames_per_super_frame = 1
    ) :
        ProtocolDecoder(payload_size, frames_per_super_frame),
        packets_per_frame_(packets_per_frame)
    { }

    virtual ~PacketProtocolDecoder() { };

    virtual const std::size_t get_frame_data_size(void) const
    {
        return (packets_per_frame_ * payload_size_);
    }

    virtual void set_packets_per_frame(std::size_t packets_per_frame)
    {
        packets_per_frame_ = packets_per_frame;
    }

    virtual const std::size_t get_packets_per_frame(void) const
    {
        return packets_per_frame_;
    }

    virtual const std::size_t get_packet_header_size(void) const = 0;

    virtual bool set_packet_received(RawFrameHeader* frame_hdr, uint32_t packet_number) = 0;
    virtual const uint32_t get_packets_received(RawFrameHeader* frame_hdr) const = 0;
    virtual const uint32_t get_packets_dropped(RawFrameHeader* frame_hdr) const = 0;
    virtual const uint8_t get_packet_state(RawFrameHeader* frame_hdr, uint32_t packet_number) const = 0;

    virtual const uint64_t get_frame_number(PacketHeader* packet_hdr) const = 0;
    virtual const uint32_t get_packet_number(PacketHeader* packet_hdr) const = 0;

    virtual const uint64_t get_image_size(RawFrameHeader* frame_hdr) const = 0;
    virtual void set_image_size(RawFrameHeader* frame_hdr, uint64_t image_size) const = 0;

    virtual SuperFrameHeader* reorder_frame(SuperFrameHeader* frame_hdr, SuperFrameHeader* reordered_frame) = 0;
    virtual SuperFrameHeader* reorder_frame(SuperFrameHeader* frame_hdr, boost::shared_ptr<FrameProcessor::Frame> reordered_frame) = 0;

protected:

    std::size_t packets_per_frame_;

};
#endif // INCLUDE_PACKET_PROTOCOL_DECODER_H_