#ifndef INCLUDE_DUMMY_DPDK_PROTOCOL_DECODER_H_
#define INCLUDE_DUMMY_DPDK_PROTOCOL_DECODER_H_

#include <ProtocolDecoder.h>
#include <rte_byteorder.h>

#include <rte_memcpy.h>

#define FRAME_OUTER_CHUNK_SIZE 10
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


struct X10GSuperFrameHeader : SuperFrameHeader 
{
    uint64_t frame_number; // Chunk number
    uint32_t frames_received; // Counter for number of frames copied into the super frame
    uint64_t super_frame_start_time; // counter for timing out super frame
    uint64_t super_frame_complete_time; 
    uint64_t super_frame_time_delta;
    uint64_t super_frame_image_size;
    uint64_t image_size;
    uint8_t frame_state[1]; // One for each frame in the superframe
}__rte_packed;

namespace Defaults
{
    const std::size_t default_packets_per_frame = PACKETS_PER_FRAME;
    const std::size_t default_payload_size = PACKET_PAYLOAD_SIZE;
}

class DummyDpdkDecoder : public ProtocolDecoder
{

public:

    DummyDpdkDecoder() :
        ProtocolDecoder(Defaults::default_packets_per_frame, Defaults::default_payload_size)
    { }

    virtual const std::size_t get_super_frame_header_size(void) const
    {
        std::size_t frame_marker_size = sizeof(X10GSuperFrameHeader().frame_state);
        std::size_t frame_header_size = sizeof(X10GSuperFrameHeader) +
            (frame_marker_size * FRAME_OUTER_CHUNK_SIZE - 1);

        return frame_header_size;
    }

    virtual const std::size_t get_super_frame_buffer_size(void) const
    {
        return sizeof(X10GSuperFrameHeader) + get_super_frame_header_size() + ((get_frame_header_size() + get_frame_data_size()) * FRAME_OUTER_CHUNK_SIZE);;
    }

    virtual const uint64_t get_super_frame_number(SuperFrameHeader* super_frame_hdr) const
    {
        return (reinterpret_cast<X10GSuperFrameHeader *>(super_frame_hdr))->frame_number;
    }

    void set_super_frame_number(SuperFrameHeader* super_frame_hdr, uint64_t frame_number)
    {
        (reinterpret_cast<X10GSuperFrameHeader *>(super_frame_hdr))->frame_number = frame_number;
    }

    const uint64_t get_super_frame_start_time(SuperFrameHeader* super_frame_hdr) const
    {
         return (reinterpret_cast<X10GSuperFrameHeader *>(super_frame_hdr))->super_frame_start_time;
    }

    void set_super_frame_start_time(SuperFrameHeader* super_frame_hdr, uint64_t start_time)
    {
        (reinterpret_cast<X10GSuperFrameHeader *>(super_frame_hdr))->super_frame_start_time = start_time;
    }

    const uint32_t get_super_frame_frames_recieved(SuperFrameHeader* super_frame_hdr) const
    {
         return (reinterpret_cast<X10GSuperFrameHeader *>(super_frame_hdr))->frames_received;
    }

    const uint8_t get_super_frame_frames_state(SuperFrameHeader* super_frame_hdr, uint32_t frame_number) const
    {
         return (reinterpret_cast<X10GSuperFrameHeader *>(super_frame_hdr))->frame_state[frame_number];
    }

    void set_super_frame_complete_time(SuperFrameHeader* super_frame_hdr, uint64_t frame_complete_time)
    {
        (reinterpret_cast<X10GSuperFrameHeader *>(super_frame_hdr))->super_frame_complete_time =
            frame_complete_time;
    }

    const uint64_t get_super_frame_complete_time(SuperFrameHeader* super_frame_hdr) const
    {
         return (reinterpret_cast<X10GSuperFrameHeader *>(super_frame_hdr))->super_frame_complete_time;
    }

    bool set_super_frame_frames_recieved(SuperFrameHeader* super_frame_hdr, uint32_t frame_number)
    {

        X10GSuperFrameHeader* X10GSuperhdr = reinterpret_cast<X10GSuperFrameHeader *>(super_frame_hdr);
        X10GSuperhdr->frame_state[frame_number] = 1;
        X10GSuperhdr->frames_received++;

        return true;
    }

    const uint64_t get_super_frame_image_size(SuperFrameHeader* super_frame_hdr) const
    {
        return (reinterpret_cast<X10GSuperFrameHeader *>(super_frame_hdr))->image_size;
    }

    void set_super_frame_image_size(SuperFrameHeader* super_frame_hdr, uint64_t image_size) const
    {
        (reinterpret_cast<X10GSuperFrameHeader *>(super_frame_hdr))->image_size = image_size;
    }

    virtual const std::size_t get_frame_header_size(void) const
    {
        std::size_t packet_marker_size = sizeof(X10GRawFrameHeader().packet_state);
        std::size_t packet_header_size = sizeof(X10GRawFrameHeader) +
            (packet_marker_size * packets_per_frame_ - 1);

        return packet_header_size;
    }

    virtual const std::size_t get_frame_data_size(void) const
    {
        return (packets_per_frame_ * payload_size_);
    }

    const std::size_t get_frame_buffer_size(void) const
    {
        return get_super_frame_header_size() + ((get_frame_header_size() + get_frame_data_size()) * FRAME_OUTER_CHUNK_SIZE);
    }

    virtual const std::size_t get_packet_header_size(void) const
    {
        return sizeof(X10GPacketHeader);
    }

    virtual const std::size_t get_frame_x_resolution(void) const
    {
        return 1000;
    }

    virtual const std::size_t get_frame_y_resolution(void) const
    {
        return 1000;
    }

    virtual const uint64_t get_frame_outer_chunk_size(void) const
    {
        return FRAME_OUTER_CHUNK_SIZE;
    }

    virtual const FrameProcessor::DataType get_frame_bit_depth(void) const
    {
        return FrameProcessor::raw_16bit;
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

    SuperFrameHeader* reorder_frame(SuperFrameHeader* frame_hdr, boost::shared_ptr<FrameProcessor::Frame> reordered_frame)
    {
        rte_memcpy(reordered_frame->get_data_ptr(),
                    reinterpret_cast<char *>(frame_hdr) + get_frame_header_size(),
                    get_frame_data_size()
                );

        return NULL;
    }

    RawFrameHeader* get_frame_header(SuperFrameHeader* superframe_hdr, uint32_t frame_number)
    {
        return reinterpret_cast<RawFrameHeader*>(
            reinterpret_cast<char *>(superframe_hdr) + get_super_frame_header_size() + (get_frame_header_size() * frame_number)
        );
    }

    char* get_image_data_start(SuperFrameHeader* superframe_hdr)
    {
        return reinterpret_cast<char *>(superframe_hdr) + get_super_frame_header_size() + (get_frame_header_size() * FRAME_OUTER_CHUNK_SIZE);
    }
};

#endif // INCLUDE_DUMMY_DPDK_PROTOCOL_DECODER_H_