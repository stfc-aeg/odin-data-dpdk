#ifndef GENERATEDDATASOURCE_H
#define GENERATEDDATASOURCE_H

#include "DataSource.h"
#include <string>
#include <rapidjson/document.h>

namespace FrameProcessor
{

class GeneratedDataSource : public DataSource
{
public:

    GeneratedDataSource(
        PacketProtocolDecoder* decoder,
        const rapidjson::Value& data_source_config);

    void getData(void* destination) override;

private:

    enum class Pattern
    {
        Incrementing,
        PacketNum,
        Fixed
    };

    Pattern pattern_;
    uint64_t max_value_;
};

}

#endif