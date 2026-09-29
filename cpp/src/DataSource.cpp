#include "DataSource.h"
#include "PacketProtocolDecoder.h"

namespace FrameProcessor
{

DataSource::DataSource(PacketProtocolDecoder* decoder, FrameProcessor::DataType data_type):
    logger_(Logger::getLogger("FP.DataSource")),
    decoder_(decoder), data_type_(data_type)
{
    if (decoder_ == nullptr)
    {
        LOG4CXX_ERROR(logger_, "DataSource constructed with a null decoder");
        return;
    }

    // Store frame geometry and packet layout from decoder
    frame_width_ = decoder_->get_frame_x_resolution();
    frame_height_ = decoder_->get_frame_y_resolution();
    frame_pixels_ = frame_width_ * frame_height_;

    packets_per_frame_ = decoder_->get_packets_per_frame();
    // payload_size_ = decoder_->get_payload_size();

    LOG4CXX_INFO(logger_, "DataSource created: " << frame_width_ << "x" << frame_height_
        << " (" << frame_pixels_ << " px), " << packets_per_frame_ << " packets/frame");
}

DataSource::~DataSource() = default;

}