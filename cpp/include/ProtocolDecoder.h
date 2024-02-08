#ifndef INCLUDE_PROTOCOL_DECODER_H_
#define INCLUDE_PROTOCOL_DECODER_H_

#include <cstddef>

struct PacketHeader { };

struct RawFrameHeader { };

struct SuperFrameHeader { };

class ProtocolDecoder
{
public:
    ProtocolDecoder(const std::size_t packets_per_frame, const std::size_t payload_size) :
        packets_per_frame_(packets_per_frame),
        payload_size_(payload_size)
    { }

    virtual ~ProtocolDecoder() { };

    virtual void set_packets_per_frame(std::size_t packets_per_frame)
    {
        packets_per_frame_ = packets_per_frame;
    }

    virtual const std::size_t get_packets_per_frame(void) const
    {
        return packets_per_frame_;
    }

    virtual void set_payload_size(std::size_t payload_size)
    {
        payload_size_ = payload_size;
    }

    virtual const std::size_t get_payload_size(void) const
    {
        return payload_size_;
    }


    // static SuperFrame virtual functions

    virtual const std::size_t get_super_frame_header_size(void) const = 0; //
    

    // Getters and Setters for superFrame metadata

    virtual const uint64_t get_super_frame_number(SuperFrameHeader* superframe_hdr) const = 0;
    virtual void set_super_frame_number(SuperFrameHeader* superframe_hdr, uint64_t frame_number) = 0;
    
    virtual const uint64_t get_super_frame_start_time(SuperFrameHeader* superframe_hdr) const = 0;
    virtual void set_super_frame_start_time(SuperFrameHeader* superframe_hdr, uint64_t start_time) = 0;
    
    virtual const uint64_t get_super_frame_complete_time(SuperFrameHeader* superframe_hdr) const = 0;
    virtual void set_super_frame_complete_time(SuperFrameHeader* superframe_hdr, uint64_t end_time) = 0;
    
    virtual const uint32_t get_super_frame_frames_recieved(SuperFrameHeader* superframe_hdr) const = 0;
    virtual bool set_super_frame_frames_recieved(SuperFrameHeader* superframe_hdr, uint32_t frame_number ) = 0;
    
    virtual const uint8_t get_super_frame_frames_state(SuperFrameHeader* superframe_hdr, uint32_t frame_number) const = 0;

    virtual RawFrameHeader* get_frame_header(SuperFrameHeader* superframe_hdr, uint32_t frame_number) = 0;
    

    virtual char* get_image_data_start(SuperFrameHeader* superframe_hdr) = 0;
    virtual const uint64_t get_super_frame_image_size(SuperFrameHeader* frame_hdr) const = 0;
    virtual void set_super_frame_image_size(SuperFrameHeader* frame_hdr, uint64_t image_size) const = 0;



    virtual const std::size_t get_frame_header_size(void) const = 0;
    virtual const std::size_t get_frame_data_size(void) const = 0;
    virtual const std::size_t get_frame_buffer_size(void) const = 0;
    virtual const std::size_t get_packet_header_size(void) const = 0;

    virtual const uint64_t get_frame_outer_chunk_size(void) const = 0;


    
    virtual const FrameProcessor::DataType get_frame_bit_depth(void) const = 0;
    virtual const std::size_t get_frame_x_resolution(void) const = 0;
    virtual const std::size_t get_frame_y_resolution(void) const = 0;

    virtual void set_frame_number(RawFrameHeader* frame_hdr, uint64_t frame_number) = 0;
    virtual const uint64_t get_frame_number(RawFrameHeader* frame_hdr) const = 0;

    virtual void set_frame_start_time(RawFrameHeader* frame_hdr, uint64_t frame_start_time) = 0;
    virtual const uint64_t get_frame_start_time(RawFrameHeader* frame_hdr) const = 0;

    virtual void set_frame_complete_time(RawFrameHeader* frame_hdr, uint64_t frame_complete_time) = 0;
    virtual const uint64_t get_frame_complete_time(RawFrameHeader* frame_hdr) const = 0;

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
    std::size_t payload_size_;

};

#endif
