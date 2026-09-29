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
    logger_ = Logger::getLogger("FP.GeneratedDataSource");

    // Parse and validate the pattern name from the JSON config
    if (!data_source_config.HasMember("pattern"))
    {
        LOG4CXX_ERROR(logger_, "GeneratedDataSource requires 'pattern'");
        throw std::runtime_error(
            "GeneratedDataSource requires 'pattern'");
    }

    if (!data_source_config["pattern"].IsString())
    {
        LOG4CXX_ERROR(logger_, "GeneratedDataSource 'pattern' must be a string");
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
        LOG4CXX_ERROR(logger_, "Unknown GeneratedDataSource pattern: " << pattern);
        throw std::runtime_error(
            "Unknown GeneratedDataSource pattern: " + pattern);
    }

    switch (data_type_)
    {
        case FrameProcessor::DataType::raw_8bit:
            max_value_ = static_cast<uint64_t>(UINT8_MAX) + 1;
            break;

        case FrameProcessor::DataType::raw_16bit:
            max_value_ = static_cast<uint64_t>(UINT16_MAX) + 1;
            break;

        case FrameProcessor::DataType::raw_32bit:
            max_value_ = static_cast<uint64_t>(UINT32_MAX) + 1;
            break;

        case FrameProcessor::DataType::raw_64bit:
            // UINT64_MAX + 1 overflows uint64_t, so this stays as the true max value.
            // Incrementing pattern wraparound will be one pixel short of ideal for
            // 64-bit frames only; acceptable given the range involved.
            max_value_ = UINT64_MAX;
            break;

        default:
            LOG4CXX_ERROR(logger_, "Unsupported frame data type");
            throw std::runtime_error(
                "Unsupported frame data type");
    }

    LOG4CXX_INFO(logger_, "GeneratedDataSource created with pattern '" << pattern
        << "', max_value: " << max_value_);
}

void GeneratedDataSource::getData(void* destination)
{
    // Compute how many pixels are in each packet
    const uint32_t pixels_per_packet = frame_pixels_ / packets_per_frame_;
    uint64_t value = 0;

    for (uint32_t pixel = 0; pixel < frame_pixels_; pixel++)
    {
        switch (pattern_)
        {
            case Pattern::Incrementing:
            {
                // Produce an oscillating ramp that wraps at max_value_
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
                // Use the packet index as the pixel value (per-packet region)
                value = pixel / pixels_per_packet;
                break;
            }

            case Pattern::Fixed:
            {
                // Fixed pattern returns a constant value for all pixels
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
                LOG4CXX_ERROR(logger_, "Unsupported frame data type");
                throw std::runtime_error("Unsupported frame data type");
        }
    }
}

DATASOURCEREGISTER(GeneratedDataSource, "generated");

}