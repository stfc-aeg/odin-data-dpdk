#ifndef GENERATEDDATASOURCE_H
#define GENERATEDDATASOURCE_H

#include "DataSource.h"
#include <string>

namespace FrameProcessor
{

class GeneratedDataSource : public DataSource
{
public:

    GeneratedDataSource(
        PacketProtocolDecoder* decoder,
        const std::string& pattern);

    void getData(uint16_t* destination) override;

private:

    enum class Pattern
    {
        Incrementing,
        PacketNum,
        Fixed
    };

    Pattern pattern_;
};

}

#endif