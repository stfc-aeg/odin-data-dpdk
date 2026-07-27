#ifndef INCLUDE_HDF5DATASOURCE_H_
#define INCLUDE_HDF5DATASOURCE_H_

#include "DataSource.h"
#include <hdf5.h>
#include <string>

namespace FrameProcessor
{

class HDF5DataSource : public DataSource
{
public:

    HDF5DataSource(
        PacketProtocolDecoder* decoder,
        const std::string& file_path
    );

    ~HDF5DataSource() override;

    void getData(uint16_t* destination) override;

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