#ifndef INCLUDE_CAMERACONTROLLER_H_
#define INCLUDE_CAMERACONTROLLER_H_
#include <vector>
#include <log4cxx/logger.h>
using namespace log4cxx;
using namespace log4cxx::helpers;
#include <DebugLevelLogger.h>
#include <IpcMessage.h>
#include <ParamContainer.h>
#include "DpdkCamera.h"
#include "camera/DpdkCameraLoader.h"
#include "DpdkSharedBuffer.h"
#include "DpdkCoreConfiguration.h"
#include "DpdkWorkerCore.h"
#include "ProtocolDecoder.h"

namespace FrameProcessor
{
    const std::string CAMERA_CONFIG_PATH = "camera";
    const std::string CAMERA_COMMAND_PATH = "command";
    const std::string CAMERA_STATUS_PATH = "status";

    // Forward declaration of the camera capture core class
    class CameraCaptureCore;
    
   
    class CameraController
    {
    public:
        typedef std::map<const std::string, CameraController*> CameraControllerMap;
        static CameraController* Instance(const std::string name)
        {
            CameraController* instance = NULL;
            CameraControllerMap::iterator iter = instances_.find(name);
            if (iter != instances_.end())
            {
                instance = iter->second;
            }
            return instance;
        }
        
        static CameraController* Instance(
            const std::string name, ProtocolDecoder* decoder, const rapidjson::Value& camera_config)
        {
            CameraController* instance = CameraController::Instance(name);
            if (instance == NULL)
            {
                instance = new CameraController(decoder, camera_config);
                instances_[name] = instance;
            }
            return instance;
        }
        
        //! Constructor for the controller taking a pointer to the decoder as an argument
        CameraController(ProtocolDecoder* decoder, const rapidjson::Value& camera_config);
       
        //! Destructor for the controller class
        ~CameraController();
        
        //! Register a capture core with the controller
        void register_capture_core(CameraCaptureCore* capture_core);
        
        //! Returns the name of the current camera state
        std::string camera_state_name(void);
       
        //! Configure the camera
        void configure(OdinData::IpcMessage& config_msg, OdinData::IpcMessage& config_reply);
       
        //! Check if camera is recording
        bool get_recording(void);
       
        //! Get the latest frame from the camera
        char* get_frame(void);
       
        //! Handle configuration request
        bool request_configuration(const std::string param_prefix, OdinData::IpcMessage& config_reply);
       
        //! Get camera status
        bool get_status(const std::string param_prefix, OdinData::IpcMessage& config_reply);
        
        //! Get current frame number from status container
        uint64_t get_frame_number();
        
        //! Get frame target from config container
        uint64_t get_frame_target();

        bool execute_command(std::string& command);
       
    private:
        static CameraControllerMap instances_;
        bool recording_;
        ProtocolDecoder* decoder_;
        boost::shared_ptr<DpdkCamera> camera_;
        LoggerPtr logger_;

        CameraCaptureCore* capture_core_;
        
        //! Get buffer for frame data
        void* get_buffer();
        
        //! Discard buffer after use
        void discard_buffer(void* buffer);
        
        //! Helper to convert JSON config to IpcMessage format
        void configure_camera_from_json(const rapidjson::Value& config, OdinData::IpcMessage& config_msg);
       
        friend class CameraCaptureCore;
    };
}
#endif // INCLUDE_CAMERACONTROLLER_H