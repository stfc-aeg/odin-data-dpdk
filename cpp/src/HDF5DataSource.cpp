#include "HDF5DataSource.h"
#include <rte_memcpy.h>
#include <stdexcept>

namespace FrameProcessor
{

HDF5DataSource::HDF5DataSource(
    PacketProtocolDecoder* decoder,
    const std::string& file_path
) :
    DataSource(decoder),
    file_path_(file_path),
    num_frames_(0),
    current_frame_(0)
{
    // Try to open the file
    file_ = H5Fopen(file_path.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT);

    if (file_ < 0)
    {
        throw std::runtime_error("Failed to open HDF5 file");
    }

    // Try to open the dataset
    dataset_ = H5Dopen2(file_, "/dummy", H5P_DEFAULT);

    if (dataset_ < 0)
    {
        H5Fclose(file_);
        throw std::runtime_error("Failed to open dataset");
    }

    // Collect dataspace shape information
    dataspace_ = H5Dget_space(dataset_);
    int ndims = H5Sget_simple_extent_ndims(dataspace_);

    // Ensure the dataset is not 1-dimensional
    if (ndims != 2 && ndims != 3)
    {
        throw std::runtime_error("Dataset must be 2D or 3D");
    }

    // Collect dimensions and load them into `dims`
    hsize_t dims[3];
    H5Sget_simple_extent_dims(dataspace_, dims, nullptr);

    // Check whether the dataset has 2D or 3D, and whether the dimensions match expectations
    if (ndims == 2)
    {
        num_frames_ = 1;

        if (dims[0] != frame_height_ || dims[1] != frame_width_)
        {
            throw std::runtime_error("Dataset dimensions do not match decoder");
        }
    }
    else
    {
        num_frames_ = dims[0];

        if (dims[1] != frame_height_ || dims[2] != frame_width_)
        {
            throw std::runtime_error("Dataset dimensions do not match decoder");
        }
    }
}

HDF5DataSource::~HDF5DataSource()
{
    H5Sclose(dataspace_);
    H5Dclose(dataset_);
    H5Fclose(file_);
}

void HDF5DataSource::getData(uint16_t* destination)
{
    hsize_t offset[3];
    hsize_t count[3];

    if (num_frames_ == 1)
    {
        H5Dread(dataset_, H5T_NATIVE_UINT16, H5S_ALL, H5S_ALL, H5P_DEFAULT, destination);
        return;
    }

    offset[0] = current_frame_;
    offset[1] = 0;
    offset[2] = 0;

    count[0] = 1;
    count[1] = frame_height_;
    count[2] = frame_width_;

    H5Sselect_hyperslab(dataspace_, H5S_SELECT_SET, offset, nullptr, count, nullptr);

    hsize_t mem_dims[2];

    mem_dims[0] = frame_height_;
    mem_dims[1] = frame_width_;

    hid_t memspace = H5Screate_simple(2, mem_dims, nullptr);

    H5Dread(dataset_, H5T_NATIVE_USHORT, memspace, dataspace_, H5P_DEFAULT, destination);
    H5Sclose(memspace);

    current_frame_++;

    if (current_frame_ >= num_frames_)
    {
        current_frame_ = 0;
    }
}

}