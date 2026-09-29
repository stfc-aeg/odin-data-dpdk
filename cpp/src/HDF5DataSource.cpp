#include "HDF5DataSource.h"
#include <stdexcept>
#include "DataSourceLoader.h"

namespace FrameProcessor
{

HDF5DataSource::HDF5DataSource(
    PacketProtocolDecoder* decoder,
    const rapidjson::Value& data_source_config) :
    DataSource(
        decoder,
        decoder->get_frame_bit_depth()),
    file_(-1),
    dataset_(-1),
    dataspace_(-1),
    file_path_(""),
    num_frames_(0),
    current_frame_(0)
{
    logger_ = Logger::getLogger("FP.HDF5DataSource");

    // Validate JSON config contains required fields for opening HDF5
    if (!data_source_config.HasMember("file_path"))
    {
        LOG4CXX_ERROR(logger_, "HDF5DataSource requires 'file_path'");
        throw std::runtime_error(
            "HDF5DataSource requires 'file_path'");
    }

    if (!data_source_config["file_path"].IsString())
    {
        LOG4CXX_ERROR(logger_, "HDF5DataSource 'file_path' must be a string");
        throw std::runtime_error(
            "HDF5DataSource 'file_path' must be a string");
    }

    if (!data_source_config.HasMember("dataset_name"))
    {
        LOG4CXX_ERROR(logger_, "HDF5DataSource requires 'dataset_name'");
        throw std::runtime_error(
            "HDF5DataSource requires 'dataset_name'");
    }

    if (!data_source_config["dataset_name"].IsString())
    {
        LOG4CXX_ERROR(logger_, "HDF5DataSource 'dataset_name' must be a string");
        throw std::runtime_error(
            "HDF5DataSource 'dataset_name' must be a string");
    }

    file_path_ = data_source_config["file_path"].GetString();

    const std::string dataset_name =
        data_source_config["dataset_name"].GetString();

    LOG4CXX_INFO(logger_, "Opening HDF5 file '" << file_path_
        << "', dataset '" << dataset_name << "'");

    // Try to open the file
    file_ = H5Fopen(file_path_.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT);

    if (file_ < 0)
    {
        LOG4CXX_ERROR(logger_, "Failed to open HDF5 file: " << file_path_);
        throw std::runtime_error("Failed to open HDF5 file");
    }

    // Try to open the dataset
    dataset_ = H5Dopen2(file_, dataset_name.c_str(), H5P_DEFAULT);

    if (dataset_ < 0)
    {
        LOG4CXX_ERROR(logger_, "Failed to open dataset: " << dataset_name);
        H5Fclose(file_);
        throw std::runtime_error("Failed to open dataset");
    }

    // Get the dataset datatype
    hid_t dataset_type = H5Dget_type(dataset_);

    if (dataset_type < 0)
    {
        LOG4CXX_ERROR(logger_, "Failed to get HDF5 dataset datatype for: " << dataset_name);
        H5Dclose(dataset_);
        H5Fclose(file_);
        throw std::runtime_error("Failed to get HDF5 dataset datatype");
    }

    // Check that the dataset contains integer data
    if (H5Tget_class(dataset_type) != H5T_INTEGER)
    {
        LOG4CXX_ERROR(logger_, "HDF5 dataset must contain integer data: " << dataset_name);
        H5Tclose(dataset_type);
        H5Dclose(dataset_);
        H5Fclose(file_);
        throw std::runtime_error("HDF5 dataset must contain integer data");
    }

    // Check that the dataset contains unsigned integer data
    if (H5Tget_sign(dataset_type) != H5T_SGN_NONE)
    {
        LOG4CXX_ERROR(logger_, "HDF5 dataset must contain unsigned integer data: " << dataset_name);
        H5Tclose(dataset_type);
        H5Dclose(dataset_);
        H5Fclose(file_);
        throw std::runtime_error("HDF5 dataset must contain unsigned integer data");
    }

    // Check that the dataset datatype matches the decoder datatype
    const size_t dataset_size = H5Tget_size(dataset_type);
    size_t expected_size;

    switch (data_type_)
    {
        case DataType::raw_8bit:
            expected_size = sizeof(uint8_t);
            break;

        case DataType::raw_16bit:
            expected_size = sizeof(uint16_t);
            break;

        case DataType::raw_32bit:
            expected_size = sizeof(uint32_t);
            break;

        case DataType::raw_64bit:
            expected_size = sizeof(uint64_t);
            break;

        default:
            LOG4CXX_ERROR(logger_, "Unsupported frame data type");
            H5Tclose(dataset_type);
            H5Dclose(dataset_);
            H5Fclose(file_);
            throw std::runtime_error("Unsupported frame data type");
    }

    if (dataset_size != expected_size)
    {
        LOG4CXX_ERROR(logger_, "HDF5 dataset datatype size (" << dataset_size
            << ") does not match decoder expected size (" << expected_size << ")");
        H5Tclose(dataset_type);
        H5Dclose(dataset_);
        H5Fclose(file_);
        throw std::runtime_error("HDF5 dataset datatype does not match decoder");
    }

    H5Tclose(dataset_type);

    // Collect dataspace shape information
    dataspace_ = H5Dget_space(dataset_);

    if (dataspace_ < 0)
    {
        LOG4CXX_ERROR(logger_, "Failed to get HDF5 dataspace for: " << dataset_name);
        H5Dclose(dataset_);
        H5Fclose(file_);
        throw std::runtime_error("Failed to get HDF5 dataspace");
    }

    const int ndims = H5Sget_simple_extent_ndims(dataspace_);

    // Dataset must be either 2D or 3D
    if (ndims != 2 && ndims != 3)
    {
        LOG4CXX_ERROR(logger_, "Dataset must be 2D or 3D, got " << ndims << "D");
        H5Sclose(dataspace_);
        H5Dclose(dataset_);
        H5Fclose(file_);
        throw std::runtime_error("Dataset must be 2D or 3D");
    }

    // Collect dimensions
    hsize_t dims[3];
    H5Sget_simple_extent_dims(dataspace_, dims, nullptr);

    // 2D dataset = single frame
    if (ndims == 2)
    {
        num_frames_ = 1;

        if (dims[0] != frame_height_ || dims[1] != frame_width_)
        {
            LOG4CXX_ERROR(logger_, "Dataset dimensions (" << dims[0] << "x" << dims[1]
                << ") do not match decoder (" << frame_height_ << "x" << frame_width_ << ")");
            H5Sclose(dataspace_);
            H5Dclose(dataset_);
            H5Fclose(file_);
            throw std::runtime_error("Dataset dimensions do not match decoder");
        }
    }
    // 3D dataset = multiple frames
    else
    {
        num_frames_ = dims[0];

        if (dims[1] != frame_height_ || dims[2] != frame_width_)
        {
            LOG4CXX_ERROR(logger_, "Dataset dimensions (" << dims[1] << "x" << dims[2]
                << ") do not match decoder (" << frame_height_ << "x" << frame_width_ << ")");
            H5Sclose(dataspace_);
            H5Dclose(dataset_);
            H5Fclose(file_);
            throw std::runtime_error("Dataset dimensions do not match decoder");
        }
    }

    LOG4CXX_INFO(logger_, "HDF5DataSource ready: " << num_frames_ << " frame(s), "
        << frame_width_ << "x" << frame_height_);
}

HDF5DataSource::~HDF5DataSource()
{
    LOG4CXX_DEBUG_LEVEL(2, logger_, "HDF5DataSource destructor");

    // Clean up HDF5 resources in reverse order of acquisition
    H5Sclose(dataspace_);
    H5Dclose(dataset_);
    H5Fclose(file_);
}

void HDF5DataSource::getData(void* destination)
{
    hid_t hdf5_type;

    switch (data_type_)
    {
        case DataType::raw_8bit:
            hdf5_type = H5T_NATIVE_UINT8;
            break;

        case DataType::raw_16bit:
            hdf5_type = H5T_NATIVE_UINT16;
            break;

        case DataType::raw_32bit:
            hdf5_type = H5T_NATIVE_UINT32;
            break;

        case DataType::raw_64bit:
            hdf5_type = H5T_NATIVE_UINT64;
            break;

        default:
            LOG4CXX_ERROR(logger_, "Unsupported frame data type");
            throw std::runtime_error("Unsupported frame data type");
    }

    // Single-frame 2D dataset
    if (num_frames_ == 1)
    {
        // Read the entire 2D dataset directly into destination buffer
        const herr_t result = H5Dread(dataset_, hdf5_type, H5S_ALL, H5S_ALL, H5P_DEFAULT, destination);

        if (result < 0)
        {
            LOG4CXX_ERROR(logger_, "Failed to read HDF5 dataset");
            throw std::runtime_error("Failed to read HDF5 dataset");
        }

        return;
    }

    // Select the current frame from the 3D dataset
    hsize_t offset[3] = {current_frame_, 0, 0};
    hsize_t count[3] = {1, frame_height_, frame_width_};

    // Select a single 2D hyperslab corresponding to the current frame index
    if (H5Sselect_hyperslab(dataspace_, H5S_SELECT_SET, offset, nullptr, count, nullptr) < 0)
    {
        LOG4CXX_ERROR(logger_, "Failed to select HDF5 frame " << current_frame_);
        throw std::runtime_error("Failed to select HDF5 frame");
    }

    // The selected frame is represented as a 2D memory dataspace
    hsize_t mem_dims[2] = {frame_height_, frame_width_};

    hid_t memspace = H5Screate_simple(2, mem_dims, nullptr);

    if (memspace < 0)
    {
        LOG4CXX_ERROR(logger_, "Failed to create HDF5 memory dataspace");
        throw std::runtime_error("Failed to create HDF5 memory dataspace");
    }

    const herr_t result = H5Dread(dataset_, hdf5_type, memspace, dataspace_, H5P_DEFAULT, destination);

    H5Sclose(memspace);

    if (result < 0)
    {
        LOG4CXX_ERROR(logger_, "Failed to read HDF5 frame " << current_frame_);
        throw std::runtime_error("Failed to read HDF5 frame");
    }

    // Move to the next frame
    current_frame_++;

    if (current_frame_ >= num_frames_)
    {
        current_frame_ = 0;
    }
}

DATASOURCEREGISTER(HDF5DataSource, "hdf5");

}