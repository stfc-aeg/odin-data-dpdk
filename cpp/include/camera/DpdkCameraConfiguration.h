#ifndef INCLUDE_DPDKCAMERACONFIGURATION_H_
#define INCLUDE_DPDKCAMERACONFIGURATION_H_

#include "ParamContainer.h"

namespace FrameProcessor
{
    namespace DpdkDefaults
    {
        const unsigned int default_camera_number = 0;
        const double default_image_timeout = 10; 
        const unsigned int default_number_frames = 0; 
        const unsigned int default_timestamp_mode = 2;
    }

    class DpdkCameraConfiguration : public OdinData::ParamContainer
    {
    public:
        // Default constructor
        DpdkCameraConfiguration() :
            ParamContainer(),
            camera_number_(DpdkDefaults::default_camera_number),
            image_timeout_(DpdkDefaults::default_image_timeout),
            num_frames_(DpdkDefaults::default_number_frames),
            timestamp_mode_(DpdkDefaults::default_timestamp_mode)
        {
            // Bind the parameters in the container
            bind_params();
        }

        // Copy constructor
        DpdkCameraConfiguration(const DpdkCameraConfiguration& config) :
            ParamContainer(config)
        {
            // Bind the parameters in the container
            bind_params();

            // Update the container from the existing config object
            update(config);
        }

    protected:
        // Bind parameters in the container
        virtual void bind_params(void)
        {
            bind_param<unsigned int>(camera_number_, "camera_number");
            bind_param<double>(image_timeout_, "image_timeout");
            bind_param<unsigned int>(num_frames_, "num_frames");
            bind_param<unsigned int>(timestamp_mode_, "timestamp_mode");
        }

        unsigned int camera_number_;     //!< Camera number as enumerated by driver
        double image_timeout_;           //!< Image acquisition timeout in seconds
        unsigned int num_frames_;        //!< Number of frames to acquire, 0 = no limit
        unsigned int timestamp_mode_;    //!< Camera timestamp mode

        friend class SimulatedDpdkCamera;
        friend class CameraCaptureCore;
    };
}

#endif // INCLUDE_DPDKCAMERACONFIGURATION_H_