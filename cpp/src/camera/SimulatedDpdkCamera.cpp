#include "camera/SimulatedDpdkCamera.h"
#include "camera/DpdkCameraLoader.h"

namespace FrameProcessor {

    SimulatedDpdkCamera::SimulatedDpdkCamera(const rapidjson::Value& camera_config) :
        DpdkCamera(camera_config),
        last_capture_time_(std::chrono::steady_clock::now()),
        logger_(Logger::getLogger("FP.SimulatedDpdkCamera"))
    {
        // Replace base class configuration and status with derived specialized types
        camera_config_ = std::make_unique<SimulatedCameraConfiguration>();
        camera_status_ = std::make_unique<SimulatedCameraStatus>();

        try
        {
            camera_config_->update(camera_config);
            LOG4CXX_INFO(logger_, "SimulatedDpdkCamera configuration updated");
        }
        catch (const std::exception& e)
        {
            LOG4CXX_ERROR(logger_, "Exception updating configuration from JSON: " << e.what());
        }

        initialize_image_generator();
    }

    SimulatedDpdkCamera::~SimulatedDpdkCamera() {
        LOG4CXX_DEBUG(logger_, "SimulatedDpdkCamera destroyed");
    }

    // Required state machine methods
    bool SimulatedDpdkCamera::connect() {
        LOG4CXX_INFO(logger_, "Connecting to simulated camera");
        
        // Ensure image generator is initialized
        if (!imageGenerator_) {
            LOG4CXX_WARN(logger_, "Image generator not initialized, attempting to reinitialize");
            initialize_image_generator();
        }
        
        sim_status()->frame_number_ = 0;

        // Log HDF5 file status
        if (imageGenerator_) {
            LOG4CXX_INFO(logger_, "Connected with HDF5 file containing " << imageGenerator_->getFrameCount() << " frames");
        } else {
            LOG4CXX_WARN(logger_, "Connected but no HDF5 image data available");
        }
        
        LOG4CXX_INFO(logger_, "Successfully connected to simulated camera");
        return true;
    }

    bool SimulatedDpdkCamera::disconnect() {
        LOG4CXX_INFO(logger_, "Disconnecting from simulated camera");
        
        sim_status()->frame_number_ = 0;
        LOG4CXX_INFO(logger_, "Successfully disconnected from simulated camera");
        return true;
    }

    bool SimulatedDpdkCamera::start_capture() {
        LOG4CXX_INFO(logger_, "Starting capture on simulated camera");
        
        sim_status()->frame_number_ = 0;
        last_capture_time_ = std::chrono::steady_clock::now();
        LOG4CXX_INFO(logger_, "Camera armed for capture");
        return true;
    }

    bool SimulatedDpdkCamera::end_capture() {
        LOG4CXX_INFO(logger_, "Ending capture on simulated camera");
        
        LOG4CXX_INFO(logger_, "Capture ended");
        return true;
    }

    bool SimulatedDpdkCamera::request_configuration(OdinData::IpcMessage& config_reply, const std::string prefix_path) {
        LOG4CXX_DEBUG(logger_, "Requesting configuration with prefix: " << prefix_path);
        
        try {
            // Encode configuration directly into the IpcMessage with the provided prefix
            OdinData::ParamContainer::Document camera_config;
            sim_config()->encode(camera_config, prefix_path);
            config_reply.update(camera_config);
            
            LOG4CXX_DEBUG(logger_, "Configuration encoded successfully");
            return true;
        }
        catch (const std::exception& e) {
            LOG4CXX_ERROR(logger_, "Exception encoding configuration: " << e.what());
            return false;
        }
    }

    bool SimulatedDpdkCamera::configure(OdinData::ParamContainer::Document& config_msg)
    {
        try
        {
            // Snapshot image-generator-affecting fields before applying the update
            auto prev_width      = sim_config()->frame_width_;
            auto prev_height     = sim_config()->frame_height_;
            auto prev_bitdepth   = sim_config()->frame_bitdepth_;
            auto prev_file_path  = sim_config()->image_file_path_;
            auto prev_dataset    = sim_config()->dataset_name_;

            sim_config()->update(config_msg);

            bool reinit_generator = (sim_config()->frame_width_     != prev_width     ||
                                     sim_config()->frame_height_    != prev_height    ||
                                     sim_config()->frame_bitdepth_  != prev_bitdepth  ||
                                     sim_config()->image_file_path_ != prev_file_path ||
                                     sim_config()->dataset_name_    != prev_dataset);

            if (reinit_generator)
            {
                initialize_image_generator();
            }
            else if (imageGenerator_)
            {
                imageGenerator_->setTextDrawingEnabled(sim_config()->enable_text_overlay_);
                imageGenerator_->setFrameNumberDrawingEnabled(sim_config()->enable_frame_number_overlay_);
            }

            LOG4CXX_INFO(logger_, "SimulatedDpdkCamera configuration updated");
            return true;
        }
        catch (const std::exception& e)
        {
            LOG4CXX_ERROR(logger_, "Exception during configuration: " << e.what());
            return false;
        }
    }

    bool SimulatedDpdkCamera::request_status(OdinData::IpcMessage& status_reply, const std::string prefix_path) {
        LOG4CXX_DEBUG(logger_, "Requesting status with prefix: " << prefix_path);
        
        try {
            // Update status with current state
            std::string state_name = get_state_name();
            sim_status()->camera_status_ = state_name;
            
            LOG4CXX_DEBUG(logger_, "Current state: " << state_name);
            LOG4CXX_DEBUG(logger_, "Frame number: " << sim_status()->frame_number_);
            
            // Encode status directly into the IpcMessage with the provided prefix
            OdinData::ParamContainer::Document camera_status;
            sim_status()->encode(camera_status, prefix_path);
            status_reply.update(camera_status);
            
            LOG4CXX_DEBUG(logger_, "Status encoded successfully");
            return true;
        }
        catch (const std::exception& e) {
            LOG4CXX_ERROR(logger_, "Exception encoding status: " << e.what());
            return false;
        }
    }

    char* SimulatedDpdkCamera::get_frame()
    {
        if (!imageGenerator_)
        {
            LOG4CXX_ERROR(logger_, "Image generator not initialized");
            return nullptr;
        }

        auto current_time = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration<double>(current_time - last_capture_time_).count();
        double frame_interval = sim_config()->frames_per_second_ > 0
            ? (1.0 / sim_config()->frames_per_second_)
            : (1.0 / 30.0);  // default to 30 FPS if not configured

        if (elapsed >= frame_interval)
        {
            last_capture_time_ = current_time;
            try
            {
                imageGenerator_->generateImage(sim_config()->camera_number_, sim_status()->frame_number_);
                return (char*)imageGenerator_->getImageData().data();
            }
            catch (const std::exception& e)
            {
                LOG4CXX_ERROR(logger_, "Exception generating frame: " << e.what());
                return nullptr;
            }
        }

        return nullptr;
    }
    
    void SimulatedDpdkCamera::initialize_image_generator()
    {
        LOG4CXX_INFO(logger_, "Initializing HDF5 image generator:"
            << " file=" << sim_config()->image_file_path_
            << " dataset=" << sim_config()->dataset_name_
            << " dims=" << sim_config()->frame_width_ << "x" << sim_config()->frame_height_
            << " fps=" << sim_config()->frames_per_second_
        );

        try
        {
            imageGenerator_ = std::make_unique<HDF5ImageLoader>(
                sim_config()->frame_width_,
                sim_config()->frame_height_,
                sim_config()->frame_bitdepth_,
                ImageFormat::MONO,
                sim_config()->dataset_name_
            );

            imageGenerator_->loadImagesFromHDF5(sim_config()->image_file_path_);
            imageGenerator_->setTextDrawingEnabled(sim_config()->enable_text_overlay_);
            imageGenerator_->setFrameNumberDrawingEnabled(sim_config()->enable_frame_number_overlay_);

            LOG4CXX_INFO(logger_, "HDF5 image generator initialized with " << imageGenerator_->getFrameCount() << " frames");
        }
        catch (const std::exception& e)
        {
            LOG4CXX_ERROR(logger_, "Failed to initialize HDF5 image generator: " << e.what());
            imageGenerator_.reset();
        }
    }

    // Register the camera class with the loader
    DPDKCAMERAREGISTER(SimulatedDpdkCamera, "SimulatedDpdkCamera");

} // namespace FrameProcessor