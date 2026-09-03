#ifndef INCLUDE_HDF5DATASOURCE_H_
#define INCLUDE_HDF5DATASOURCE_H_

#include "DataSource.h"
#include <hdf5.h>
#include <rapidjson/document.h>
#include <string>

namespace FrameProcessor
{

class HDF5DataSource : public DataSource
{
public:

    HDF5DataSource(
        PacketProtocolDecoder* decoder,
        const rapidjson::Value& data_source_config);

    ~HDF5DataSource() override;

    void getData(void* destination) override;

private:

    hid_t file_;
    hid_t dataset_;
    hid_t dataspace_;

    std::string file_path_;

    uint32_t num_frames_;
    uint32_t current_frame_;
};

}

#endif