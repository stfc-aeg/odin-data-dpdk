#ifndef INCLUDE_PROTOCOL_DECODER_H_
#define INCLUDE_PROTOCOL_DECODER_H_

#include <cstdint>
#include <cstddef>
#include <vector>
#include "dpdk_version_compatibiliy.h"

struct RawFrameHeader { };

struct __rte_packed_begin SuperFrameHeader
{
    uint64_t super_frame_number; // Chunk number
    uint64_t super_frame_start_time; // counter for timing out super frame
    uint64_t super_frame_complete_time;
    uint64_t super_frame_image_size;
    uint32_t frames_received; // Counter for number of frames copied into the super frame
    uint8_t frame_state[];   //!< Flexible array member - length depends on number of frames
} __rte_packed_end;

class ProtocolDecoder
{
public:

    ProtocolDecoder(const std::size_t payload_size = 0, unsigned int frames_per_super_frame = 1) :
        payload_size_(payload_size),
        frames_per_super_frame_(frames_per_super_frame),
        frame_bit_depth_(FrameProcessor::DataType::raw_unknown),
        frame_x_resolution_(0),
        frame_y_resolution_(0)
    {

    }

    virtual ~ProtocolDecoder() { }

    virtual void set_payload_size(const std::size_t payload_size)
    {
        payload_size_ = payload_size;
    }

    virtual const std::size_t get_payload_size(void) const
    {
        return payload_size_;
    }

    virtual const std::size_t get_super_frame_header_size(void) const
    {
        const std::size_t frame_state_size = sizeof(SuperFrameHeader().frame_state[0]);
        return sizeof(SuperFrameHeader) + (frame_state_size * (frames_per_super_frame_ - 1));
    }
    
    virtual const std::size_t get_super_frame_buffer_size(void) const
    {
        return get_super_frame_header_size() + 
            ((get_frame_header_size() + get_frame_data_size()) * frames_per_super_frame_);;
    }

    virtual const uint64_t get_super_frame_number(SuperFrameHeader* super_frame_hdr) const
    {
        return super_frame_hdr->super_frame_number;
    } 

    virtual void set_super_frame_number(SuperFrameHeader* super_frame_hdr, uint64_t super_frame_number)
    {
        super_frame_hdr->super_frame_number = super_frame_number;
    }
    
    virtual const uint64_t get_super_frame_start_time(SuperFrameHeader* super_frame_hdr) const
    {
        return super_frame_hdr->super_frame_start_time;
    }

    virtual void set_super_frame_start_time(SuperFrameHeader* super_frame_hdr, uint64_t start_time)
    {
        super_frame_hdr->super_frame_start_time = start_time;
    }
    
    virtual const uint64_t get_super_frame_complete_time(SuperFrameHeader* super_frame_hdr) const
    {
        return super_frame_hdr->super_frame_complete_time;
    }

    virtual void set_super_frame_complete_time(SuperFrameHeader* super_frame_hdr, uint64_t complete_time)
    {
        super_frame_hdr->super_frame_complete_time = complete_time;
    }
    
    virtual const uint64_t get_super_frame_image_size(SuperFrameHeader* super_frame_hdr) const
    {
        return super_frame_hdr->super_frame_image_size;
    }

    virtual void set_super_frame_image_size(SuperFrameHeader* super_frame_hdr, uint64_t image_size)
    {
        super_frame_hdr->super_frame_image_size = image_size;
    }

    virtual const uint32_t get_super_frame_frames_received(SuperFrameHeader* super_frame_hdr) const
    {
        return super_frame_hdr->frames_received;
    }

    virtual bool set_super_frame_frames_received(SuperFrameHeader* super_frame_hdr, uint32_t frame_number)
    {
        if (frame_number >= frames_per_super_frame_)
        {
            return false;
        }
        super_frame_hdr->frame_state[frame_number] = 1;
        super_frame_hdr->frames_received++;

        return true;
    }
    
    virtual const uint8_t get_super_frame_frame_state(SuperFrameHeader* super_frame_hdr, uint32_t frame_number)
    {
        if (frame_number >= frames_per_super_frame_)
        {
            return 0;
        }
        else
        {
            return super_frame_hdr->frame_state[frame_number];
        }
    }

    const std::size_t get_frame_buffer_size(void) const
    {
        return get_super_frame_header_size() + 
            ((get_frame_header_size() + get_frame_data_size()) * frames_per_super_frame_);
    }

    virtual const uint64_t get_frame_outer_chunk_size(void) const
    {
        return frames_per_super_frame_;
    }

    virtual const FrameProcessor::DataType get_frame_bit_depth(void) const
    {
        return frame_bit_depth_;
    }

    virtual const std::size_t get_frame_x_resolution(void) const
    {
        return frame_x_resolution_;
    }

    virtual const std::size_t get_frame_y_resolution(void) const
    {
        return frame_y_resolution_;
    }

    virtual RawFrameHeader* get_frame_header(SuperFrameHeader* super_frame_hdr, uint32_t frame_number)
    {
        return reinterpret_cast<RawFrameHeader*>(
            reinterpret_cast<char *>(super_frame_hdr) + 
            get_super_frame_header_size() + (get_frame_header_size() * frame_number)
        );
    }

    virtual char* get_image_data_start(SuperFrameHeader* super_frame_hdr)
    {
        return reinterpret_cast<char *>(super_frame_hdr) + get_image_data_offset();
    }

    virtual std::size_t get_image_data_offset(void)
    {
        return get_super_frame_header_size() + (get_frame_header_size() * frames_per_super_frame_);
    }


    // Abstract frame methods that must be implemented by application-specific decoders
    virtual const std::size_t get_frame_header_size(void) const = 0;
    virtual const std::size_t get_frame_data_size(void) const = 0;
    
    virtual void set_frame_number(RawFrameHeader* frame_hdr, uint64_t frame_number) = 0;
    virtual const uint64_t get_frame_number(RawFrameHeader* frame_hdr) const = 0;

    virtual void set_frame_start_time(RawFrameHeader* frame_hdr, uint64_t frame_start_time) = 0;
    virtual const uint64_t get_frame_start_time(RawFrameHeader* frame_hdr) const = 0;

    virtual void set_frame_complete_time(RawFrameHeader* frame_hdr, uint64_t frame_complete_time) = 0;
    virtual const uint64_t get_frame_complete_time(RawFrameHeader* frame_hdr) const = 0;

    virtual std::vector<std::size_t> get_frame_dimensions(void) const
    {
        std::vector<std::size_t> dims;
        dims.push_back(frame_x_resolution_);
        dims.push_back(frame_y_resolution_);
        return dims;
    }

protected:
    std::size_t payload_size_;    
    unsigned int frames_per_super_frame_;
    FrameProcessor::DataType frame_bit_depth_;
    std::size_t frame_x_resolution_;
    std::size_t frame_y_resolution_;
};


#endif // INCLUDE_PROTOCOL_DECODER_H_
