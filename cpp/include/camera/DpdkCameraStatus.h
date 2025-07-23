#ifndef INCLUDE_DPDKCAMERASTATUS_H_
#define INCLUDE_DPDKCAMERASTATUS_H_

#include "ParamContainer.h"

namespace FrameProcessor
{
    namespace StatusDefaults
    {
        const unsigned int frame_number = 0; 
        const std::string camera_status = "disconnected"; 
    }

    /**
     * @class DpdkCameraStatus
     * @brief Base status container for DPDK cameras
     *
     * This class provides a container for common camera status information
     * that can be encoded to/decoded from JSON for monitoring and control.
     */
    class DpdkCameraStatus : public OdinData::ParamContainer
    {
    public:
        /**
         * @brief Default constructor
         *
         * Initializes all parameters to default values and binds them
         * in the parameter container.
         */
        DpdkCameraStatus() :
            ParamContainer(),
            frame_number_(StatusDefaults::frame_number),
            camera_status_(StatusDefaults::camera_status)
        {
            // Bind the parameters in the container
            bind_params();
        }

        /**
         * @brief Copy constructor
         *
         * Creates a copy of an existing status object. All parameters
         * are first bound and then the underlying parameter container 
         * is updated from the existing object.
         *
         * @param status - existing status object to copy
         */
        DpdkCameraStatus(const DpdkCameraStatus& status) :
            ParamContainer(status)
        {
            // Bind the parameters in the container
            bind_params();

            // Update the container from the existing status object
            update(status);
        }

    protected:
        /**
         * @brief Bind parameters in the container
         *
         * Binds all parameters to named paths in the container.
         */
        virtual void bind_params(void)
        {
            bind_param<std::string>(camera_status_, "camera_status");
            bind_param<unsigned int>(frame_number_, "frame_number");
        }

        std::string camera_status_;     //!< Camera status as reported by the state machine
        unsigned int frame_number_;     //!< Latest captured frame number 

        // Friend class declarations for classes that need direct access
        friend class DpdkCamera;
        friend class CameraCaptureCore;
    };
}

#endif // INCLUDE_DPDKCAMERASTATUS_H_