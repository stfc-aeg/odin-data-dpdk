#ifndef INCLUDE_DUMMY_DPDK_PROTOCOL_DECODER_H_
#define INCLUDE_DUMMY_DPDK_PROTOCOL_DECODER_H_

#include <PacketProtocolDecoder.h>
#include <rte_byteorder.h>
#include <rte_memcpy.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_udp.h>
#include <boost/shared_ptr.hpp>
#include <map>
#include <string>
#include "dpdk_version_compatibiliy.h"

// Mode enumeration — add new modes here as needed
enum class DummyDpdkMode {
    FRAME_1000x1000,   // 250 packets, 8000 byte payload, 1000x1000 uint16 frame
    FRAME_512x512,     // 64 packets,  4096 byte payload, 512x512  uint16 frame
    FRAME_256x256      // 16 packets,  2048 byte payload, 256x256  uint16 frame
};

struct DummyModeConfiguration {
    std::size_t packets_per_frame;
    std::size_t payload_size;
    std::size_t frame_outer_chunk_size;
    FrameProcessor::DataType bit_depth;
    std::size_t x_resolution;
    std::size_t y_resolution;
};

struct __rte_packed_begin X10GPacketHeader : PacketHeader
{
    rte_be64_t frame_number;
    rte_be64_t padding[6];
    rte_be32_t packet_number;
    uint8_t markers;
    uint8_t _unused_1;
    uint8_t padding_bytes;
    uint8_t readout_lane;
} __rte_packed_end;

struct __rte_packed_begin X10GRawFrameHeader : RawFrameHeader
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
} __rte_packed_end;

class DummyDpdkDecoder : public PacketProtocolDecoder
{

public:

    static const std::map<std::string, DummyDpdkMode>& get_mode_string_map()
    {
        static const std::map<std::string, DummyDpdkMode> mode_string_map = {
            {"frame_1000x1000", DummyDpdkMode::FRAME_1000x1000},
            {"frame_512x512",   DummyDpdkMode::FRAME_512x512},
            {"frame_256x256",   DummyDpdkMode::FRAME_256x256}
        };
        return mode_string_map;
    }

    DummyDpdkDecoder(DummyDpdkMode initial_mode = DummyDpdkMode::FRAME_1000x1000) :
        PacketProtocolDecoder(
            get_mode_configs().at(initial_mode).packets_per_frame,
            get_mode_configs().at(initial_mode).payload_size,
            get_mode_configs().at(initial_mode).frame_outer_chunk_size
        ),
        current_mode_(initial_mode)
    {
        configure_for_mode(initial_mode);
    }

    void set_mode(DummyDpdkMode new_mode)
    {
        if (new_mode != current_mode_)
        {
            current_mode_ = new_mode;
            configure_for_mode(new_mode);
        }
    }

    DummyDpdkMode get_mode() const { return current_mode_; }

    std::string get_mode_string() const
    {
        for (const auto& kv : get_mode_string_map())
        {
            if (kv.second == current_mode_) return kv.first;
        }
        return "unknown";
    }

    // Resolve a mode string to its configuration, falling back to the current mode if empty
    // or unrecognised. Const and stateless — safe to call from multiple threads.
    const DummyModeConfiguration& resolve_mode(const std::string& mode) const
    {
        if (!mode.empty())
        {
            const auto& mode_map = get_mode_string_map();
            auto it = mode_map.find(mode);
            if (it != mode_map.end())
                return get_mode_configs().at(it->second);
            // Unknown mode string — log once (static flag avoids flooding)
            static bool warned = false;
            if (!warned) {
                fprintf(stderr, "DummyDpdkDecoder: unknown mode '%s', falling back to current mode\n",
                        mode.c_str());
                warned = true;
            }
        }
        return get_mode_configs().at(current_mode_);
    }

    virtual const std::size_t get_payload_size(const std::string& mode = "") const override
    {
        return resolve_mode(mode).payload_size;
    }

    virtual const std::size_t get_packets_per_frame(const std::string& mode = "") const override
    {
        return resolve_mode(mode).packets_per_frame;
    }

    virtual const uint64_t get_frame_outer_chunk_size(const std::string& mode = "") const override
    {
        return resolve_mode(mode).frame_outer_chunk_size;
    }

    virtual const FrameProcessor::DataType get_frame_bit_depth(const std::string& mode = "") const override
    {
        return resolve_mode(mode).bit_depth;
    }

    virtual const std::size_t get_frame_x_resolution(const std::string& mode = "") const override
    {
        return resolve_mode(mode).x_resolution;
    }

    virtual const std::size_t get_frame_y_resolution(const std::string& mode = "") const override
    {
        return resolve_mode(mode).y_resolution;
    }

    virtual std::vector<std::size_t> get_frame_dimensions(const std::string& mode = "") const override
    {
        const DummyModeConfiguration& cfg = resolve_mode(mode);
        return {cfg.x_resolution, cfg.y_resolution};
    }

    virtual const std::size_t get_frame_header_size(const std::string& mode = "") const override
    {
        std::size_t packet_marker_size = sizeof(X10GRawFrameHeader().packet_state);
        std::size_t n_packets = resolve_mode(mode).packets_per_frame;
        return sizeof(X10GRawFrameHeader) + (packet_marker_size * n_packets - 1);
    }

    virtual const std::size_t get_packet_header_size(void) const
    {
        return sizeof(X10GPacketHeader);
    }

    virtual const std::size_t get_packet_payload_offset(void) const
    {
        // Headers are always before payload in dummy decoder
        return sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr) + 
                sizeof(struct rte_udp_hdr) + get_packet_header_size();
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

    bool set_packet_number(PacketHeader* packet_hdr, uint32_t packet_number) {
        X10GPacketHeader* x10g_hdr = reinterpret_cast<X10GPacketHeader*>(packet_hdr);
        x10g_hdr->packet_number = packet_number;
        return true;
    }

    bool set_packet_frame_number(PacketHeader* packet_hdr, rte_be64_t frame_number) {
        X10GPacketHeader* x10g_hdr = reinterpret_cast<X10GPacketHeader*>(packet_hdr);
        x10g_hdr->frame_number = frame_number;
        return true;
    }

    void prepare_frame(uint16_t* raw_frame, uint16_t* prepared_frame)
    {
        return;
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
                    reinterpret_cast<char *>(frame_hdr) + get_frame_header_size(get_mode_string()),
                    get_frame_data_size(get_mode_string())
                );

        return NULL;
    }

private:

    DummyDpdkMode current_mode_;

    static const std::map<DummyDpdkMode, DummyModeConfiguration>& get_mode_configs()
    {
        static const std::map<DummyDpdkMode, DummyModeConfiguration> mode_configs = {
            //                                packets  payload  chunk  bit_depth                            x     y
            {DummyDpdkMode::FRAME_1000x1000, {250,     8000,    10,     FrameProcessor::DataType::raw_16bit, 1000, 1000}},
            {DummyDpdkMode::FRAME_512x512,   {64,      4096,    10,     FrameProcessor::DataType::raw_16bit, 512,  512}},
            {DummyDpdkMode::FRAME_256x256,   {16,      2048,    10,     FrameProcessor::DataType::raw_16bit, 256,  256}}
        };
        return mode_configs;
    }

    void configure_for_mode(DummyDpdkMode mode)
    {
        const DummyModeConfiguration& cfg = get_mode_configs().at(mode);
        packets_per_frame_      = cfg.packets_per_frame;
        payload_size_           = cfg.payload_size;
        frames_per_super_frame_ = cfg.frame_outer_chunk_size;
        frame_bit_depth_        = cfg.bit_depth;
        frame_x_resolution_     = cfg.x_resolution;
        frame_y_resolution_     = cfg.y_resolution;
    }

};

#endif // INCLUDE_DUMMY_DPDK_PROTOCOL_DECODER_H_