#include "GeneratedDataSource.h"
#include "PacketProtocolDecoder.h"
#include <cstdint>
#include <stdexcept>
#include <rapidjson/document.h>
#include "DataSourceLoader.h"

namespace FrameProcessor
{

GeneratedDataSource::GeneratedDataSource(
    PacketProtocolDecoder* decoder,
    const rapidjson::Value& data_source_config) :
    DataSource(
        decoder,
        decoder->get_frame_bit_depth())
{
    if (!data_source_config.HasMember("pattern"))
    {
        throw std::runtime_error(
            "GeneratedDataSource requires 'pattern'");
    }

    if (!data_source_config["pattern"].IsString())
    {
        throw std::runtime_error(
            "GeneratedDataSource 'pattern' must be a string");
    }

    const std::string pattern =
        data_source_config["pattern"].GetString();

    if (pattern == "incrementing")
    {
        pattern_ = Pattern::Incrementing;
    }
    else if (pattern == "packetnum")
    {
        pattern_ = Pattern::PacketNum;
    }
    else if (pattern == "fixed")
    {
        pattern_ = Pattern::Fixed;
    }
    else
    {
        throw std::runtime_error(
            "Unknown GeneratedDataSource pattern: " + pattern);
    }

    switch (data_type_)
    {
        case FrameProcessor::DataType::raw_8bit:
            max_value_ = UINT8_MAX;
            break;

        case FrameProcessor::DataType::raw_16bit:
            max_value_ = UINT16_MAX;
            break;

        case FrameProcessor::DataType::raw_32bit:
            max_value_ = UINT32_MAX;
            break;

        case FrameProcessor::DataType::raw_64bit:
            max_value_ = UINT64_MAX;
            break;

        default:
            throw std::runtime_error(
                "Unsupported frame data type");
    }
}

void GeneratedDataSource::getData(void* destination)
{
    const uint32_t pixels_per_packet = frame_pixels_ / packets_per_frame_;
    uint64_t value = 0;

    for (uint32_t pixel = 0; pixel < frame_pixels_; pixel++)
    {
        switch (pattern_)
        {
            case Pattern::Incrementing:
            {
                if ((pixel/max_value_) % 2 == 0)
                {
                    value = pixel % max_value_;
                }
                else
                {
                    value = max_value_ - (pixel % max_value_);
                }
                break;
            }

            case Pattern::PacketNum:
            {
                value = pixel / pixels_per_packet;
                break;
            }

            case Pattern::Fixed:
            {
                value = 84;
                break;
            }
        }

        switch (data_type_)
        {
            case FrameProcessor::DataType::raw_8bit:
                static_cast<uint8_t*>(destination)[pixel] = static_cast<uint8_t>(value);
                break;

            case FrameProcessor::DataType::raw_16bit:
                static_cast<uint16_t*>(destination)[pixel] = static_cast<uint16_t>(value);
                break;

            case FrameProcessor::DataType::raw_32bit:
                static_cast<uint32_t*>(destination)[pixel] = static_cast<uint32_t>(value);
                break;

            case FrameProcessor::DataType::raw_64bit:
                static_cast<uint64_t*>(destination)[pixel] = value;
                break;

            default:
                throw std::runtime_error("Unsupported frame data type");
        }
    }
}

DATASOURCEREGISTER(GeneratedDataSource, "generated");

}