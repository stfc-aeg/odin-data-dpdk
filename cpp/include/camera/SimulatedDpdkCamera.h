#ifndef SIMULATED_DPDK_CAMERA_H
#define SIMULATED_DPDK_CAMERA_H

#include "camera/DpdkCamera.h"
#include "camera/SimulatedCameraConfiguration.h"
#include "camera/SimulatedCameraStatus.h"
#include "camera/HDF5ImageLoader.h"
#include <log4cxx/logger.h>
using namespace log4cxx;
using namespace log4cxx::helpers;
#include <DebugLevelLogger.h>
#include <memory>
#include <vector>
#include <chrono>
#include <rapidjson/document.h>

namespace FrameProcessor
{
    /**
     * @class SimulatedDpdkCamera
     * @brief Simulated camera implementation for testing and development
     *
     * This class provides a simulated camera interface that generates
     * synthetic frames for testing the camera framework without requiring
     * actual hardware.
     */
    class SimulatedDpdkCamera : public DpdkCamera
    {
    public:
        /**
         * @brief Default constructor for SimulatedDpdkCamera
         *
         * Initializes the camera with default configuration and status
         */
        SimulatedDpdkCamera();
        
        /**
         * @brief Constructor for SimulatedDpdkCamera with configuration
         *
         * Initializes the camera with configuration from JSON
         *
         * @param camera_config - JSON configuration object
         */
        SimulatedDpdkCamera(const rapidjson::Value& camera_config);
        
        /**
         * @brief Destructor
         *
         * Ensures proper cleanup of resources
         */
        ~SimulatedDpdkCamera() override;
        
        // State machine required methods
        bool connect() override;
        bool disconnect() override;
        bool start_capture() override;
        bool end_capture() override;
        
        // Other camera interface methods
        bool request_configuration(OdinData::IpcMessage& config_reply, const std::string prefix_path) override;
        bool configure(OdinData::ParamContainer::Document& config_msg) override;
        bool request_status(OdinData::IpcMessage& status_reply, const std::string prefix_path) override;
        char* get_frame() override;
        
    protected:
        /**
         * @brief Helper method to access the derived status object
         * @return Pointer to SimulatedCameraStatus
         */
        SimulatedCameraStatus* sim_status() {
            return static_cast<SimulatedCameraStatus*>(camera_status_.get());
        }
        
        /**
         * @brief Helper method to access the derived config object
         * @return Pointer to SimulatedCameraConfiguration
         */
        SimulatedCameraConfiguration* sim_config() {
            return static_cast<SimulatedCameraConfiguration*>(camera_config_.get());
        }
        
    private:
        // Image generation component
        std::unique_ptr<HDF5ImageLoader> imageGenerator_;
        
        // Frame rate control
        std::chrono::steady_clock::time_point last_capture_time_;
        
        // Initialize the image generator
        void initialize_image_generator();

        LoggerPtr logger_;
        
        
    };
}

#endif // SIMULATED_DPDK_CAMERA_H