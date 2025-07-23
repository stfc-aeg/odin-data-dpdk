#include "DpdkCamera.h"
#include "camera/CameraStateMachine.h"
#include <iostream>

namespace FrameProcessor
{
    DpdkCamera::DpdkCamera(const rapidjson::Value& camera_config)
    {
        // Initialize logger
        logger_ = Logger::getLogger("FP.DpdkCamera");
        
        // Create the state machine
        state_machine_ = std::make_unique<CameraStateMachine>(this);
        
        // Initialize configuration and status containers with base types
        // Derived classes will replace these with their specialized versions
        camera_config_ = std::make_unique<DpdkCameraConfiguration>();
        camera_status_ = std::make_unique<DpdkCameraStatus>();
        
        // Update configuration from JSON if provided
        if (camera_config.IsObject()) {
            try {
                camera_config_->update(camera_config);
                LOG4CXX_DEBUG(logger_, "Base camera configuration updated from JSON");
            }
            catch (const std::exception& e) {
                LOG4CXX_WARN(logger_, "Failed to update base configuration from JSON: " << e.what());
            }
        }
        
        LOG4CXX_DEBUG(logger_, "DpdkCamera base class initialized");
    }
    
    DpdkCamera::~DpdkCamera()
    {
        // Destructor - cleanup handled by unique_ptrs
        LOG4CXX_DEBUG(logger_, "DpdkCamera destroyed");
    }
    
    bool DpdkCamera::execute_command(std::string& command)
    {
        // Delegate to the state machine
        try {
            if (state_machine_) {
                state_machine_->execute_command(command);
                return true;
            } else {
                LOG4CXX_ERROR(logger_, "State machine not initialized");
                return false;
            }
        } catch (const std::runtime_error& e) {
            // Log the error
            LOG4CXX_ERROR(logger_, "Error executing command: " << e.what());
            return false;
        }
    }
    
    std::string DpdkCamera::get_state_name()
    {
        if (state_machine_) {
            return state_machine_->current_state_name();
        } else {
            return "no_state_machine";
        }
    }
}