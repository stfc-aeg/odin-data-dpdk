#include "camera/DpdkCamera.h"
#include "camera/CameraStateMachine.h"

namespace FrameProcessor
{
    DpdkCamera::DpdkCamera(const rapidjson::Value& camera_config)
    {
        logger_ = Logger::getLogger("FP.DpdkCamera");
        state_machine_ = std::make_unique<CameraStateMachine>(this);

        // Derived classes replace these with specialized versions
        camera_config_ = std::make_unique<DpdkCameraConfiguration>();
        camera_status_ = std::make_unique<DpdkCameraStatus>();

        if (camera_config.IsObject())
        {
            try
            {
                camera_config_->update(camera_config);
            }
            catch (const std::exception& e)
            {
                LOG4CXX_WARN(logger_, "Failed to update base configuration from JSON: " << e.what());
            }
        }
    }

    DpdkCamera::~DpdkCamera()
    {
        LOG4CXX_DEBUG(logger_, "DpdkCamera destroyed");
    }

    bool DpdkCamera::execute_command(std::string& command)
    {
        try
        {
            if (!state_machine_)
            {
                LOG4CXX_ERROR(logger_, "State machine not initialized");
                return false;
            }
            state_machine_->execute_command(command);
            return true;
        }
        catch (const std::runtime_error& e)
        {
            LOG4CXX_ERROR(logger_, "Error executing command: " << e.what());
            return false;
        }
    }

    std::string DpdkCamera::get_state_name()
    {
        return state_machine_ ? state_machine_->current_state_name() : "no_state_machine";
    }
}