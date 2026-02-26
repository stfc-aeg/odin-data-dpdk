#ifndef INCLUDE_DUMMY_DPDK_CAMERA_PROTOCOL_DECODER_H_
#define INCLUDE_DUMMY_DPDK_CAMERA_PROTOCOL_DECODER_H_

#include "camera/CameraProtocolDecoder.h"
#include <rte_byteorder.h>
#include <rte_memcpy.h>
#include "DpdkWorkerCore.h"
#include "dpdk_version_compatibiliy.h"

#define FRAME_OUTER_CHUNK_SIZE 1
#define PACKETS_PER_FRAME 1

struct __rte_packed_begin DummyDpdkCameraFrameHeader : RawFrameHeader
{
    uint64_t frame_number;
    uint32_t packets_received;
    uint32_t sof_marker_count;
    uint32_t eof_marker_count;
    uint64_t frame_start_time;
    uint64_t frame_complete_time;
    uint32_t frame_time_delta;
    uint64_t image_size;
    uint8_t packet_state[PACKETS_PER_FRAME];  // Flexible array member
} __rte_packed_end;

namespace Defaults
{
    const std::size_t default_packets_per_frame = PACKETS_PER_FRAME;
    const std::size_t default_payload_size = 18874368;  // OrcaQuest image size (2304 * 4096 * 2 bytes per pixel)
}

class DummyDpdkCameraProtocolDecoder : public CameraProtocolDecoder
{
public:
    DummyDpdkCameraProtocolDecoder() :
        CameraProtocolDecoder(Defaults::default_payload_size, Defaults::default_packets_per_frame)
    { }

    virtual ~DummyDpdkCameraProtocolDecoder() { }

    // Implement pure virtual methods from ProtocolDecoder base class
    
    /**
     * @brief Get the size of the frame header
     * @return Size in bytes of the frame header including packet state array
     */
    virtual const std::size_t get_frame_header_size(void) const override
    {
        std::size_t packet_marker_size = sizeof(DummyDpdkCameraFrameHeader().packet_state);
        std::size_t frame_header_size = sizeof(DummyDpdkCameraFrameHeader) +
            (packet_marker_size * PACKETS_PER_FRAME - 1);
        return frame_header_size;
    }

    /**
     * @brief Set the frame number in the frame header
     * @param frame_hdr Pointer to the raw frame header
     * @param frame_number Frame number to set
     */
    virtual void set_frame_number(RawFrameHeader* frame_hdr, uint64_t frame_number) override
    {
        (reinterpret_cast<DummyDpdkCameraFrameHeader *>(frame_hdr))->frame_number = frame_number;
    }

    /**
     * @brief Get the frame number from the frame header
     * @param frame_hdr Pointer to the raw frame header
     * @return Frame number
     */
    virtual const uint64_t get_frame_number(RawFrameHeader* frame_hdr) const override
    {
        return (reinterpret_cast<DummyDpdkCameraFrameHeader *>(frame_hdr))->frame_number;
    }

    /**
     * @brief Set the frame start time in the frame header
     * @param frame_hdr Pointer to the raw frame header
     * @param frame_start_time Start time to set
     */
    virtual void set_frame_start_time(RawFrameHeader* frame_hdr, uint64_t frame_start_time) override
    {
        (reinterpret_cast<DummyDpdkCameraFrameHeader *>(frame_hdr))->frame_start_time = frame_start_time;
    }

    /**
     * @brief Get the frame start time from the frame header
     * @param frame_hdr Pointer to the raw frame header
     * @return Frame start time
     */
    virtual const uint64_t get_frame_start_time(RawFrameHeader* frame_hdr) const override
    {
        return (reinterpret_cast<DummyDpdkCameraFrameHeader *>(frame_hdr))->frame_start_time;
    }

    /**
     * @brief Set the frame complete time in the frame header
     * @param frame_hdr Pointer to the raw frame header
     * @param frame_complete_time Complete time to set
     */
    virtual void set_frame_complete_time(RawFrameHeader* frame_hdr, uint64_t frame_complete_time) override
    {
        (reinterpret_cast<DummyDpdkCameraFrameHeader *>(frame_hdr))->frame_complete_time = frame_complete_time;
    }

    /**
     * @brief Get the frame complete time from the frame header
     * @param frame_hdr Pointer to the raw frame header
     * @return Frame complete time
     */
    virtual const uint64_t get_frame_complete_time(RawFrameHeader* frame_hdr) const override
    {
        return (reinterpret_cast<DummyDpdkCameraFrameHeader *>(frame_hdr))->frame_complete_time;
    }

    // Additional Orca-specific methods
    
    /**
     * @brief Get frame dimensions for OrcaQuest camera
     */
    virtual const std::size_t get_frame_x_resolution(void) const
    {
        return 4096; // OrcaQuest X resolution
    }

    virtual const std::size_t get_frame_y_resolution(void) const
    {
        return 2304;  // OrcaQuest Y resolution
    }

    /**
     * @brief Get the data type/bit depth for OrcaQuest frames
     */
    virtual const FrameProcessor::DataType get_frame_bit_depth(void) const
    {
        return FrameProcessor::raw_16bit;  // OrcaQuest uses 16-bit data
    }

    /**
     * @brief Get the image size from frame header
     */
    const uint64_t get_image_size(RawFrameHeader* frame_hdr) const
    {
        return (reinterpret_cast<DummyDpdkCameraFrameHeader *>(frame_hdr))->image_size;
    }

    /**
     * @brief Set the image size in frame header
     */
    void set_image_size(RawFrameHeader* frame_hdr, uint64_t image_size)
    {
        (reinterpret_cast<DummyDpdkCameraFrameHeader *>(frame_hdr))->image_size = image_size;
    }

    /**
     * @brief Get number of packets received for this frame
     */
    const uint32_t get_packets_received(RawFrameHeader* frame_hdr) const
    {
        return (reinterpret_cast<DummyDpdkCameraFrameHeader *>(frame_hdr))->packets_received;
    }

    /**
     * @brief Mark a packet as received
     */
    bool set_packet_received(RawFrameHeader* frame_hdr, uint32_t packet_number)
    {
        if (packet_number >= PACKETS_PER_FRAME)
        {
            return false;
        }
        
        DummyDpdkCameraFrameHeader* orca_hdr = reinterpret_cast<DummyDpdkCameraFrameHeader *>(frame_hdr);
        orca_hdr->packet_state[packet_number] = 1;
        orca_hdr->packets_received++;
        return true;
    }

    /**
     * @brief Get the state of a specific packet
     */
    const uint8_t get_packet_state(RawFrameHeader* frame_hdr, uint32_t packet_number) const
    {
        return (reinterpret_cast<DummyDpdkCameraFrameHeader *>(frame_hdr))->packet_state[packet_number];
    }

};

#endif // INCLUDE_DUMMY_DPDK_CAMERA_PROTOCOL_DECODER_H_