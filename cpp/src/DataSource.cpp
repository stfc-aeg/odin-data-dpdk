#include "DataSource.h"
#include "PacketProtocolDecoder.h"

namespace FrameProcessor
{

DataSource::DataSource(PacketProtocolDecoder* decoder, FrameProcessor::DataType data_type):
    decoder_(decoder), data_type_(data_type)
{
    // Store frame geometry and packet layout from decoder
    frame_width_ = decoder_->get_frame_x_resolution();
    frame_height_ = decoder_->get_frame_y_resolution();
    frame_pixels_ = frame_width_ * frame_height_;

    packets_per_frame_ = decoder_->get_packets_per_frame();
    // payload_size_ = decoder_->get_payload_size();
}

DataSource::~DataSource() = default;

}