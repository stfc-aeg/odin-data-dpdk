#include "camera/CameraController.h"

namespace FrameProcessor {

    CameraController::CameraControllerMap CameraController::instances_;

CameraController::CameraController(
    ProtocolDecoder* decoder,
    const rapidjson::Value& camera_config
) :
    decoder_(decoder),
    logger_(Logger::getLogger("FR.CameraController")),
    recording_(false),
    capture_core_(NULL),
    camera_(NULL)
{
    LOG4CXX_INFO(logger_, "Core CameraController creation!");
    
    try {
        // Extract camera name from configuration
        if (!camera_config.HasMember("camera_name")) {
            LOG4CXX_ERROR(logger_, "Missing 'camera_name' in camera configuration");
            throw std::runtime_error("Missing 'camera_name' in camera configuration");
        }
        
        if (!camera_config["camera_name"].IsString()) {
            LOG4CXX_ERROR(logger_, "'camera_name' must be a string in camera configuration");
            throw std::runtime_error("'camera_name' must be a string in camera configuration");
        }
        
        std::string camera_name = camera_config["camera_name"].GetString();
        LOG4CXX_INFO(logger_, "Loading camera class: " << camera_name);
        
        // Load the camera using DpdkCameraLoader with the extracted name
        camera_ = FrameProcessor::DpdkCameraLoader<FrameProcessor::DpdkCamera>::load_class(camera_name, camera_config);
        
        if (!camera_) {
            LOG4CXX_ERROR(logger_, "Failed to load camera class: " << camera_name);
            throw std::runtime_error("Failed to load camera class: " + camera_name);
        }
        
        LOG4CXX_INFO(logger_, "Successfully loaded camera: " << camera_name);
    }
    catch (const std::exception& e) {
        LOG4CXX_ERROR(logger_, "Exception during camera loading: " << e.what());
        throw;
    }
}

// Destructor
CameraController::~CameraController()
{
    // Clean up resources if needed
    if (camera_) {
        if (recording_) {
            camera_->end_capture();
        }
        camera_->disconnect();
    }
}

// Execute command
bool CameraController::execute_command(std::string& command)
{
    return camera_->execute_command(command);
}

void CameraController::register_capture_core(CameraCaptureCore* capture_core)
{
    capture_core_ = capture_core;
}


// Configure the camera
void CameraController::configure(OdinData::IpcMessage& config_msg, OdinData::IpcMessage& config_reply)
{

    // Update camera config
    if (config_msg.has_param(CAMERA_CONFIG_PATH))
    {
        LOG4CXX_DEBUG_LEVEL(2, logger_, "Configure request has camera configuration");
        OdinData::ParamContainer::Document config_doc;
        config_msg.encode_params(config_doc, CAMERA_CONFIG_PATH);

        if (!camera_->configure(config_doc))
        {
            config_reply.set_nack("Camera configuration failed");
        }

        
    }

    // Send camera a command
    if (config_msg.has_param(CAMERA_COMMAND_PATH))
    {
        std::string command = config_msg.get_param<std::string>(CAMERA_COMMAND_PATH);
        LOG4CXX_DEBUG_LEVEL(2, logger_, "Configure request has command: " << command);
        
        if (!camera_->execute_command(command))
        {
            config_reply.set_nack("Camera " + command + " command failed");
        }
    }

}

// Check if camera is recording
bool CameraController::get_recording()
{
    return recording_;
}

// Get the latest frame from the camera
char* CameraController::get_frame()
{
    if (!camera_) {
        LOG4CXX_DEBUG(logger_, "Cannot get frame: No camera!");
        return nullptr;
    }
    
    try {
        // Get frame data using the get_frame_data() method in DpdkCamera
        LOG4CXX_DEBUG(logger_, "Getting frame.");
        return camera_->get_frame();
    }
    catch (const std::exception& e) {
        LOG4CXX_ERROR(logger_, "Exception getting frame: " << e.what());
        return nullptr;
    }
}

// Handle configuration request
bool CameraController::request_configuration(const std::string param_prefix, OdinData::IpcMessage& config_reply)
{
    if (!camera_) {
        config_reply.set_nack("Camera not created");
        return false;
    }
    
    // Build the full path for camera configuration
    std::string camera_config_prefix = param_prefix;
    if (!camera_config_prefix.empty() && camera_config_prefix.back() != '/') {
        camera_config_prefix += "/";
    }
    camera_config_prefix += CAMERA_CONFIG_PATH;
    
    // Get configuration from camera update config_reply directly
    if (!camera_->request_configuration(config_reply, camera_config_prefix)) {
        config_reply.set_nack("Failed to get camera configuration");
        return false;
    }
    
    // Don't update the reply again - the camera has already done it
    return true;
}

// Get camera status
bool CameraController::get_status(const std::string param_prefix, OdinData::IpcMessage& config_reply)
{
    if (!camera_) {
        config_reply.set_nack("Camera not created");
        return false;
    }
    
    // Build the full path for camera status
    std::string camera_status_prefix = param_prefix;
    if (!camera_status_prefix.empty() && camera_status_prefix.back() != '/') {
        camera_status_prefix += "/";
    }
    camera_status_prefix += CAMERA_STATUS_PATH;  // Assuming this constant exists
    
    // Get status from camera - it will update config_reply directly
    if (!camera_->request_status(config_reply, camera_status_prefix)) {
        config_reply.set_nack("Failed to get camera status");
        return false;
    }
    
    // Don't update the reply again - the camera has already done it
    return true;
}

} // namespace FrameProcessor