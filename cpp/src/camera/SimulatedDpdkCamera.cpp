#include "camera/SimulatedDpdkCamera.h"
#include "camera/DpdkCameraLoader.h"

namespace FrameProcessor {

    SimulatedDpdkCamera::SimulatedDpdkCamera(const rapidjson::Value& camera_config) :
        DpdkCamera(camera_config),  // Call base constructor with config
        last_capture_time_(std::chrono::steady_clock::now()),
        logger_(Logger::getLogger("FP.SimulatedDpdkCamera"))
    {
        // Replace base class configuration and status with derived types
        camera_config_ = std::make_unique<SimulatedCameraConfiguration>();
        camera_status_ = std::make_unique<SimulatedCameraStatus>();
        
        // Update logger name for derived class
        logger_ = Logger::getLogger("FP.SimulatedDpdkCamera");
        
        // Update configuration from JSON
        try {
            camera_config_->update(camera_config);
            LOG4CXX_INFO(logger_, "SimulatedDpdkCamera configuration updated from JSON");
            
            // Log the configured values for debugging
            LOG4CXX_DEBUG(logger_, "Camera number: " << sim_config()->camera_number_);
            LOG4CXX_DEBUG(logger_, "Number of frames: " << sim_config()->num_frames_);
            LOG4CXX_DEBUG(logger_, "Image timeout: " << sim_config()->image_timeout_);
            LOG4CXX_DEBUG(logger_, "Timestamp mode: " << sim_config()->timestamp_mode_);
            LOG4CXX_DEBUG(logger_, "Frame dimensions: " << sim_config()->frame_width_ << "x" << sim_config()->frame_height_);
            LOG4CXX_DEBUG(logger_, "Image file path: " << sim_config()->image_file_path_);
            LOG4CXX_DEBUG(logger_, "Dataset name: " << sim_config()->dataset_name_);
        }
        catch (const std::exception& e) {
            LOG4CXX_ERROR(logger_, "Exception updating configuration from JSON: " << e.what());
            // Continue with default configuration
        }
        
        // Initialize the image generator
        initialize_image_generator();
        
        LOG4CXX_DEBUG(logger_, "SimulatedDpdkCamera constructed with JSON configuration");
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
        
        // Update status
        sim_status()->camera_status_ = "connected";
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
        
        // Update status
        sim_status()->camera_status_ = "disconnected";
        sim_status()->frame_number_ = 0;
        
        LOG4CXX_INFO(logger_, "Successfully disconnected from simulated camera");
        return true;
    }

    bool SimulatedDpdkCamera::start_capture() {
        LOG4CXX_INFO(logger_, "Starting capture on simulated camera");
        
        // Reset frame counter and timing
        sim_status()->frame_number_ = 0;
        last_capture_time_ = std::chrono::steady_clock::now();
        
        // Update status
        sim_status()->camera_status_ = "capturing";
        
        LOG4CXX_INFO(logger_, "Camera armed for capture");
        return true;
    }

    bool SimulatedDpdkCamera::end_capture() {
        LOG4CXX_INFO(logger_, "Ending capture on simulated camera");
        
        // Update status
        sim_status()->camera_status_ = "connected";
        
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

    bool SimulatedDpdkCamera::configure(OdinData::ParamContainer::Document& config_msg) {
        LOG4CXX_DEBUG(logger_, "Configuring simulated camera");
        
        try {
            // Create a copy of current configuration
            SimulatedCameraConfiguration new_config(*sim_config());
            
            // Update configuration from the message
            new_config.update(config_msg);
            
            // Compare configurations and apply changes
            bool config_changed = false;
            
            // Check camera number
            if (new_config.camera_number_ != sim_config()->camera_number_) {
                LOG4CXX_INFO(logger_, "Camera number updated from " << sim_config()->camera_number_ 
                            << " to " << new_config.camera_number_);
                sim_config()->camera_number_ = new_config.camera_number_;
                config_changed = true;
            }
            
            // Check image timeout
            if (new_config.image_timeout_ != sim_config()->image_timeout_) {
                LOG4CXX_INFO(logger_, "Image timeout updated from " << sim_config()->image_timeout_ 
                            << " to " << new_config.image_timeout_);
                sim_config()->image_timeout_ = new_config.image_timeout_;
                config_changed = true;
            }
            
            // Check number of frames
            if (new_config.num_frames_ != sim_config()->num_frames_) {
                LOG4CXX_INFO(logger_, "Number of frames updated from " << sim_config()->num_frames_ 
                            << " to " << new_config.num_frames_);
                sim_config()->num_frames_ = new_config.num_frames_;
                config_changed = true;
            }
            
            // Check timestamp mode
            if (new_config.timestamp_mode_ != sim_config()->timestamp_mode_) {
                LOG4CXX_INFO(logger_, "Timestamp mode updated from " << sim_config()->timestamp_mode_ 
                            << " to " << new_config.timestamp_mode_);
                sim_config()->timestamp_mode_ = new_config.timestamp_mode_;
                config_changed = true;
            }
            
            // Check frame dimensions
            if (new_config.frame_width_ != sim_config()->frame_width_) {
                LOG4CXX_INFO(logger_, "Frame width updated from " << sim_config()->frame_width_ 
                            << " to " << new_config.frame_width_);
                sim_config()->frame_width_ = new_config.frame_width_;
                config_changed = true;
                // Reinitialize image generator if dimensions changed
                initialize_image_generator();
            }
            
            if (new_config.frame_height_ != sim_config()->frame_height_) {
                LOG4CXX_INFO(logger_, "Frame height updated from " << sim_config()->frame_height_ 
                            << " to " << new_config.frame_height_);
                sim_config()->frame_height_ = new_config.frame_height_;
                config_changed = true;
                // Reinitialize image generator if dimensions changed
                initialize_image_generator();
            }
            
            if (new_config.frame_bitdepth_ != sim_config()->frame_bitdepth_) {
                LOG4CXX_INFO(logger_, "Frame bit depth updated from " << sim_config()->frame_bitdepth_ 
                            << " to " << new_config.frame_bitdepth_);
                sim_config()->frame_bitdepth_ = new_config.frame_bitdepth_;
                config_changed = true;
                // Reinitialize image generator if bit depth changed
                initialize_image_generator();
            }
            
            // Check HDF5 file path
            if (new_config.image_file_path_ != sim_config()->image_file_path_) {
                LOG4CXX_INFO(logger_, "Image file path updated from " << sim_config()->image_file_path_ 
                            << " to " << new_config.image_file_path_);
                sim_config()->image_file_path_ = new_config.image_file_path_;
                config_changed = true;
                // Reinitialize image generator with new file
                initialize_image_generator();
            }
            
            // Check dataset name
            if (new_config.dataset_name_ != sim_config()->dataset_name_) {
                LOG4CXX_INFO(logger_, "Dataset name updated from " << sim_config()->dataset_name_ 
                            << " to " << new_config.dataset_name_);
                sim_config()->dataset_name_ = new_config.dataset_name_;
                config_changed = true;
                // Reinitialize image generator with new dataset
                initialize_image_generator();
            }
            
            // Check frames per second
            if (new_config.frames_per_second_ != sim_config()->frames_per_second_) {
                LOG4CXX_INFO(logger_, "Frames per second updated from " << sim_config()->frames_per_second_ 
                            << " to " << new_config.frames_per_second_);
                sim_config()->frames_per_second_ = new_config.frames_per_second_;
                config_changed = true;
            }
            
            // Check text overlay settings
            if (new_config.enable_text_overlay_ != sim_config()->enable_text_overlay_) {
                LOG4CXX_INFO(logger_, "Text overlay enabled updated from " << sim_config()->enable_text_overlay_ 
                            << " to " << new_config.enable_text_overlay_);
                sim_config()->enable_text_overlay_ = new_config.enable_text_overlay_;
                config_changed = true;
                // Update image generator overlay settings
                if (imageGenerator_) {
                    imageGenerator_->setTextDrawingEnabled(sim_config()->enable_text_overlay_);
                }
            }
            
            if (new_config.enable_frame_number_overlay_ != sim_config()->enable_frame_number_overlay_) {
                LOG4CXX_INFO(logger_, "Frame number overlay enabled updated from " << sim_config()->enable_frame_number_overlay_ 
                            << " to " << new_config.enable_frame_number_overlay_);
                sim_config()->enable_frame_number_overlay_ = new_config.enable_frame_number_overlay_;
                config_changed = true;
                // Update image generator overlay settings
                if (imageGenerator_) {
                    imageGenerator_->setFrameNumberDrawingEnabled(sim_config()->enable_frame_number_overlay_);
                }
            }
            
            if (config_changed) {
                LOG4CXX_INFO(logger_, "Configuration updated successfully");
            } else {
                LOG4CXX_DEBUG(logger_, "No configuration changes detected");
            }
            
            return true;
        }
        catch (const std::exception& e) {
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

    char* SimulatedDpdkCamera::get_frame() {
        if (!imageGenerator_) {
            LOG4CXX_ERROR(logger_, "Image generator not initialized");
            return nullptr;
        }
        
        // Timer to gate valid frames to a given framerate
        auto current_time = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration<double>(current_time - last_capture_time_).count();

        // Use frames_per_second from configuration for frame rate control
        double frame_interval = (sim_config()->frames_per_second_ > 0) ? 
                               (1.0 / sim_config()->frames_per_second_) : (1.0 / 30.0); // Default to 30 FPS
        
        if (elapsed >= (frame_interval)) {
            last_capture_time_ = current_time;

            try {
                imageGenerator_->generateImage(sim_config()->camera_number_, sim_status()->frame_number_);

                return (char*) imageGenerator_->getImageData().data();
            }
            catch (const std::exception& e) {
                LOG4CXX_ERROR(logger_, "Exception generating frame: " << e.what());
                return nullptr;
            }
        }

        return nullptr;
    }
    
    void SimulatedDpdkCamera::initialize_image_generator() {
        double frame_interval = (sim_config()->frames_per_second_ > 0) ? (1.0 / sim_config()->frames_per_second_) : (1.0 / 30.0); // Default to 30 FPS
        LOG4CXX_INFO(logger_, "Initializing HDF5 image generator");
        LOG4CXX_INFO(logger_, "HDF5 file path: " << sim_config()->image_file_path_);
        LOG4CXX_INFO(logger_, "Dataset name: " << sim_config()->dataset_name_);
        LOG4CXX_INFO(logger_, "Frame dimensions: " << sim_config()->frame_width_ << "x" << sim_config()->frame_height_);
        LOG4CXX_INFO(logger_, "Frame rate: " << sim_config()->frames_per_second_);
        LOG4CXX_INFO(logger_, "frame interval: " << frame_interval);

        
        try {
            // Create the HDF5 image loader with configurable dimensions
            imageGenerator_ = std::make_unique<HDF5ImageLoader>(
                sim_config()->frame_width_,     // width from config
                sim_config()->frame_height_,    // height from config  
                sim_config()->frame_bitdepth_,  // bit depth from config
                ImageFormat::MONO,              // format
                sim_config()->dataset_name_     // dataset name from config
            );
            
            // Load the HDF5 file
            imageGenerator_->loadImagesFromHDF5(sim_config()->image_file_path_);
            
            // Enable text drawing features based on configuration
            imageGenerator_->setTextDrawingEnabled(sim_config()->enable_text_overlay_);
            imageGenerator_->setFrameNumberDrawingEnabled(sim_config()->enable_frame_number_overlay_);
            
            LOG4CXX_INFO(logger_, "HDF5 image generator initialized successfully with " 
                        << imageGenerator_->getFrameCount() << " frames");
        }
        catch (const std::exception& e) {
            LOG4CXX_ERROR(logger_, "Failed to initialize HDF5 image generator: " << e.what());
            LOG4CXX_WARN(logger_, "Camera will continue without image generator - frames will return nullptr");
            // Set imageGenerator_ to nullptr to ensure get_frame() behaves correctly
            imageGenerator_.reset();
        }
    }

    // Register the camera class with the loader
    DPDKCAMERAREGISTER(SimulatedDpdkCamera, "SimulatedDpdkCamera");

} // namespace FrameProcessor