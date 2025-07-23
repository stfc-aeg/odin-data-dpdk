#ifndef INCLUDE_DUMMY_DPDK_PROTOCOL_DECODER_H_
#define INCLUDE_DUMMY_DPDK_PROTOCOL_DECODER_H_

#include <PacketProtocolDecoder.h>
#include <rte_byteorder.h>
#include <rte_memcpy.h>

#define FRAME_OUTER_CHUNK_SIZE 1
#define PACKETS_PER_FRAME 250
#define PACKET_PAYLOAD_SIZE 8000

struct X10GPacketHeader : PacketHeader
{
    rte_be64_t frame_number;
    rte_be64_t padding[6];
    rte_be32_t packet_number;
    uint8_t markers;
    uint8_t _unused_1;
    uint8_t padding_bytes;
    uint8_t readout_lane;
} __rte_packed;

struct X10GRawFrameHeader : RawFrameHeader
{
    uint64_t frame_number;
    uint32_t packets_received;
    uint32_t sof_marker_count;
    uint32_t eof_marker_count;
    uint64_t frame_start_time;
	uint64_t frame_complete_time;
	uint32_t frame_time_delta;
    uint64_t image_size;
    uint8_t packet_state[1];  // One for each packet in the frame
} __rte_packed;

namespace Defaults
{
    const std::size_t default_packets_per_frame = PACKETS_PER_FRAME;
    const std::size_t default_payload_size = PACKET_PAYLOAD_SIZE;
}

class DummyDpdkDecoder : public PacketProtocolDecoder
{

public:

    DummyDpdkDecoder() :
        PacketProtocolDecoder(
            Defaults::default_packets_per_frame, Defaults::default_payload_size,
            FRAME_OUTER_CHUNK_SIZE
        )
    {
        frame_bit_depth_ = FrameProcessor::DataType::raw_16bit;
        frame_x_resolution_ = 1000;
        frame_y_resolution_ = 1000;
    }

    virtual const std::size_t get_frame_header_size(void) const
    {
        std::size_t packet_marker_size = sizeof(X10GRawFrameHeader().packet_state);
        std::size_t packet_header_size = sizeof(X10GRawFrameHeader) +
            (packet_marker_size * packets_per_frame_ - 1);

        return packet_header_size;
    }

    virtual const std::size_t get_packet_header_size(void) const
    {
        return sizeof(X10GPacketHeader);
    }

    void set_frame_number(RawFrameHeader* frame_hdr, uint64_t frame_number)
    {
        (reinterpret_cast<X10GRawFrameHeader *>(frame_hdr))->frame_number = frame_number;
    }

    const uint64_t get_frame_number(RawFrameHeader* frame_hdr) const
    {
        return (reinterpret_cast<X10GRawFrameHeader *>(frame_hdr))->frame_number;
    }

    void set_frame_start_time(RawFrameHeader* frame_hdr, uint64_t frame_start_time)
    {
        (reinterpret_cast<X10GRawFrameHeader *>(frame_hdr))->frame_start_time = frame_start_time;
    }

    const uint64_t get_image_size(RawFrameHeader* frame_hdr) const
    {
        return (reinterpret_cast<X10GRawFrameHeader *>(frame_hdr))->image_size;
    }

    void set_image_size(RawFrameHeader* frame_hdr, uint64_t image_size) const
    {
        (reinterpret_cast<X10GRawFrameHeader *>(frame_hdr))->image_size = image_size;
    }

    const uint64_t get_frame_start_time(RawFrameHeader* frame_hdr) const
    {
         return (reinterpret_cast<X10GRawFrameHeader *>(frame_hdr))->frame_start_time;
    }

    void set_frame_complete_time(RawFrameHeader* frame_hdr, uint64_t frame_complete_time)
    {
        (reinterpret_cast<X10GRawFrameHeader *>(frame_hdr))->frame_complete_time =
            frame_complete_time;
    }

    const uint64_t get_frame_complete_time(RawFrameHeader* frame_hdr) const
    {
         return (reinterpret_cast<X10GRawFrameHeader *>(frame_hdr))->frame_complete_time;
    }

    bool set_packet_received(RawFrameHeader* frame_hdr, uint32_t packet_number)
    {

        if (packet_number >= packets_per_frame_)
        {
            return false;
        }
        else
        {
            X10GRawFrameHeader* x10g_hdr = reinterpret_cast<X10GRawFrameHeader *>(frame_hdr);
            x10g_hdr->packet_state[packet_number] = 1;
            x10g_hdr->packets_received++;

            return true;
        }
    }

    const uint32_t get_packets_received(RawFrameHeader* frame_hdr) const
    {
        return (reinterpret_cast<X10GRawFrameHeader *>(frame_hdr))->packets_received;
    }

    const uint32_t get_packets_dropped(RawFrameHeader* frame_hdr) const
    {
        return packets_per_frame_ -
            (reinterpret_cast<X10GRawFrameHeader *>(frame_hdr))->packets_received;
    }

    const uint8_t get_packet_state(RawFrameHeader* frame_hdr, uint32_t packet_number) const
    {
        return (reinterpret_cast<X10GRawFrameHeader *>(frame_hdr))->packet_state[packet_number];
    }

    const uint64_t get_frame_number(PacketHeader* packet_hdr) const
    {
        return (reinterpret_cast<X10GPacketHeader *>(packet_hdr))->frame_number;
    }

    const uint32_t get_packet_number(PacketHeader* packet_hdr) const
    {
        return rte_bswap32((reinterpret_cast<X10GPacketHeader *>(packet_hdr))->packet_number);
    }

    SuperFrameHeader* reorder_frame(SuperFrameHeader* frame_hdr, SuperFrameHeader* reordered_frame)
    {
        return frame_hdr;
    }

    SuperFrameHeader* reorder_frame(
        SuperFrameHeader* frame_hdr, boost::shared_ptr<FrameProcessor::Frame> reordered_frame
    )
    {
        rte_memcpy(reordered_frame->get_data_ptr(),
                    reinterpret_cast<char *>(frame_hdr) + get_frame_header_size(),
                    get_frame_data_size()
                );

        return NULL;
    }

};

#endif // INCLUDE_DUMMY_DPDK_PROTOCOL_DECODER_H_