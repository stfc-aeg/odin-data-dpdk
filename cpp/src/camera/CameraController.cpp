#include "camera/CameraController.h"

// CameraController.h only forward declares CameraCaptureCore (the two headers include each other's
// types), but get_buffer/discard_buffer call through to it, so the full definition is needed here.
#include "camera/CameraCaptureCore.h"

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
    if (!camera_config.HasMember("camera_name") || !camera_config["camera_name"].IsString())
    {
        LOG4CXX_ERROR(logger_, "Missing or invalid 'camera_name' in camera configuration");
        throw std::runtime_error("Missing or invalid 'camera_name' in camera configuration");
    }

    std::string camera_name = camera_config["camera_name"].GetString();
    LOG4CXX_INFO(logger_, "Loading camera class: " << camera_name);

    camera_ = FrameProcessor::DpdkCameraLoader<FrameProcessor::DpdkCamera>::load_class(camera_name, camera_config);

    if (!camera_)
    {
        LOG4CXX_ERROR(logger_, "Failed to load camera class: " << camera_name);
        throw std::runtime_error("Failed to load camera class: " + camera_name);
    }

    LOG4CXX_INFO(logger_, "Successfully loaded camera: " << camera_name);
}

CameraController::~CameraController()
{
    if (camera_)
    {
        if (recording_) camera_->end_capture();
        camera_->disconnect();
    }
}

bool CameraController::execute_command(std::string& command)
{
    return camera_->execute_command(command);
}

void CameraController::register_capture_core(CameraCaptureCore* capture_core)
{
    capture_core_ = capture_core;

    // Bind the buffer management functions into the camera now that a capture core owns the
    // clear_frames ring. Cameras that write frame data directly into shared buffer memory call
    // these rather than reaching for the ring themselves, which keeps the DPDK details out of the
    // camera classes. Bound here rather than in the constructor because the capture core is not
    // known until it registers.
    if (camera_)
    {
        camera_->getBuffer = boost::bind(&CameraController::get_buffer, this);
        camera_->discardBuffer = boost::bind(&CameraController::discard_buffer, this, boost::placeholders::_1);
    }
}

void* CameraController::get_buffer()
{
    // No capture core means no clear_frames ring to draw from
    if (capture_core_ == NULL)
    {
        LOG4CXX_ERROR(logger_, "Cannot get buffer: no capture core registered");
        return nullptr;
    }

    // Returns nullptr when the pool is exhausted; the caller decides whether to drop the frame
    return capture_core_->pop_empty_buffer();
}

void CameraController::discard_buffer(void* buffer)
{
    if (capture_core_ == NULL)
    {
        LOG4CXX_ERROR(logger_, "Cannot discard buffer: no capture core registered");
        return;
    }

    if (buffer == NULL)
    {
        LOG4CXX_DEBUG(logger_, "Ignoring request to discard a null buffer");
        return;
    }

    capture_core_->push_empty_buffer(buffer);
}


void CameraController::configure(OdinData::IpcMessage& config_msg, OdinData::IpcMessage& config_reply)
{
    if (config_msg.has_param(CAMERA_CONFIG_PATH))
    {
        OdinData::ParamContainer::Document config_doc;
        config_doc.SetObject();
        config_msg.copy_params(config_doc, CAMERA_CONFIG_PATH);

        if (!camera_->configure(config_doc))
        {
            config_reply.set_nack("Camera configuration failed");
        }
    }

    if (config_msg.has_param(CAMERA_COMMAND_PATH))
    {
        std::string command = config_msg.get_param<std::string>(CAMERA_COMMAND_PATH);
        if (!camera_->execute_command(command))
        {
            config_reply.set_nack("Camera " + command + " command failed");
        }
    }
}

bool CameraController::get_recording()
{
    return recording_;
}

char* CameraController::get_frame()
{
    if (!camera_)
    {
        LOG4CXX_DEBUG(logger_, "Cannot get frame: no camera");
        return nullptr;
    }
    try
    {
        return camera_->get_frame();
    }
    catch (const std::exception& e)
    {
        LOG4CXX_ERROR(logger_, "Exception getting frame: " << e.what());
        return nullptr;
    }
}

bool CameraController::request_configuration(const std::string param_prefix, OdinData::IpcMessage& config_reply)
{
    if (!camera_)
    {
        config_reply.set_nack("Camera not created");
        return false;
    }

    std::string prefix = param_prefix;
    if (!prefix.empty() && prefix.back() != '/') prefix += "/";
    prefix += CAMERA_CONFIG_PATH;

    if (!camera_->request_configuration(config_reply, prefix))
    {
        config_reply.set_nack("Failed to get camera configuration");
        return false;
    }
    return true;
}

bool CameraController::get_status(const std::string param_prefix, OdinData::IpcMessage& config_reply)
{
    if (!camera_)
    {
        config_reply.set_nack("Camera not created");
        return false;
    }

    std::string prefix = param_prefix;
    if (!prefix.empty() && prefix.back() != '/') prefix += "/";
    prefix += CAMERA_STATUS_PATH;

    if (!camera_->request_status(config_reply, prefix))
    {
        config_reply.set_nack("Failed to get camera status");
        return false;
    }
    return true;
}

} // namespace FrameProcessor