#ifndef DPDK_CAMERA_H
#define DPDK_CAMERA_H

#include <cstdint>
#include <string>
#include <memory>
#include <rapidjson/document.h>

#include <log4cxx/logger.h>
using namespace log4cxx;
using namespace log4cxx::helpers;
#include <DebugLevelLogger.h>

#include <boost/function.hpp>
#include "ParamContainer.h"
#include "IpcMessage.h"
#include "camera/DpdkCameraConfiguration.h"
#include "camera/DpdkCameraStatus.h"

namespace FrameProcessor
{
    // Forward declaration
    class CameraStateMachine;

    /**
     * @class DpdkCamera
     * @brief Abstract base class for camera implementations
     */
    class DpdkCamera
    {
    public:
        /**
         * @brief Constructor with configuration
         * @param camera_config - JSON configuration object
         */
        DpdkCamera(const rapidjson::Value& camera_config);
        
        /**
         * @brief Virtual destructor
         */
        virtual ~DpdkCamera();
        
        
        /**
         * @brief Get the current state name of the camera
         * @return String representation of the current state
         */
        std::string get_state_name();

        //---------------------------------------------------------------
        // Required interface methods
        //---------------------------------------------------------------
        
        /**
         * @brief Connect to the camera
         * @return true if connection successful, false otherwise
         */
        virtual bool connect() = 0;
        
        /**
         * @brief Disconnect from the camera
         * @return true if disconnection successful, false otherwise
         */
        virtual bool disconnect() = 0;
        
        /**
         * @brief Start capturing frames
         * @return true if capture started successfully, false otherwise
         */
        virtual bool start_capture() = 0;
        
        /**
         * @brief End the capture process
         * @return true if capture ended successfully, false otherwise
         */
        virtual bool end_capture() = 0;
        
        //---------------------------------------------------------------
        // Other camera interface methods
        //---------------------------------------------------------------

        bool execute_command(std::string& command);

        virtual bool request_configuration(OdinData::IpcMessage& config_reply, const std::string prefix_path) = 0;
        virtual bool configure(OdinData::ParamContainer::Document& config_msg) = 0;
        virtual bool request_status(OdinData::IpcMessage& status_reply, const std::string prefix_path) = 0;
        virtual char* get_frame() = 0;
        
    // Allow access to classes that are derived from this class
    protected:
        // Bound buffer management functions
        boost::function<void*()> getBuffer;
        boost::function<void(void*)> discardBuffer;

        // Configuration and status containers
        std::unique_ptr<DpdkCameraConfiguration> camera_config_;
        std::unique_ptr<DpdkCameraStatus> camera_status_;

        // Statemachine for camera commands
        std::unique_ptr<CameraStateMachine> state_machine_;

        LoggerPtr logger_;
        
    private:
        
        friend class CameraStateMachine;
        friend class CameraController;
        friend class CameraCaptureCore;
    };
}

#endif // DPDK_CAMERA_H