#ifndef INCLUDE_SIMULATEDCAMERACONFIGURATION_H_
#define INCLUDE_SIMULATEDCAMERACONFIGURATION_H_

#include "camera/DpdkCameraConfiguration.h"

namespace FrameProcessor
{
    namespace SimulatedDefaults
    {
        const unsigned int default_frame_height = 2304;
        const unsigned int default_frame_width = 4096;
        const unsigned int default_frame_bitdepth = 16;
        const std::string default_image_file_path = "/aeg_sw/work/users/mrl93834/develop/projects/odin-data-dpdk/install/test_data_000000.h5";
        const std::string default_dataset_name = "default";
        const unsigned int default_frames_per_second = 60;
        const bool default_enable_text_overlay = true;
        const bool default_enable_frame_number_overlay = true;
    }

    /**
     * @class SimulatedCameraConfiguration
     * @brief Configuration class for simulated cameras
     *
     * This class extends the basic DPDK camera configuration with
     * parameters specific to simulated cameras, such as frame dimensions,
     * bit depth, HDF5 file path, and text overlay options.
     */
    class SimulatedCameraConfiguration : public DpdkCameraConfiguration
    {
    public:
        /**
         * @brief Default constructor
         *
         * Initialize all parameters to their default values and bind them
         * in the parameter container for JSON access.
         */
        SimulatedCameraConfiguration() :
            DpdkCameraConfiguration(),
            frame_height_(SimulatedDefaults::default_frame_height),
            frame_width_(SimulatedDefaults::default_frame_width),
            frame_bitdepth_(SimulatedDefaults::default_frame_bitdepth),
            image_file_path_(SimulatedDefaults::default_image_file_path),
            dataset_name_(SimulatedDefaults::default_dataset_name),
            frames_per_second_(SimulatedDefaults::default_frames_per_second),
            enable_text_overlay_(SimulatedDefaults::default_enable_text_overlay),
            enable_frame_number_overlay_(SimulatedDefaults::default_enable_frame_number_overlay)
        {
            // Bind the parameters in the container
            bind_params();
        }

        /**
         * @brief Copy constructor
         *
         * Create a new configuration from an existing one.
         * @param config - reference to an existing configuration to copy
         */
        SimulatedCameraConfiguration(const SimulatedCameraConfiguration& config) :
            DpdkCameraConfiguration(config)
        {
            // Bind the parameters in the container
            bind_params();

            // Update the container from the existing config object
            update(config);
        }

    protected:
        /**
         * @brief Bind parameters in the container
         *
         * Override the base class method to bind both the common camera
         * parameters and the simulation-specific parameters.
         */
        virtual void bind_params(void) override
        {
            // First bind the base class parameters
            DpdkCameraConfiguration::bind_params();

            // Then bind the simulation-specific parameters
            bind_param<unsigned int>(frame_height_, "frame_height");
            bind_param<unsigned int>(frame_width_, "frame_width");
            bind_param<unsigned int>(frame_bitdepth_, "frame_bitdepth");
            bind_param<std::string>(image_file_path_, "image_file_path");
            bind_param<std::string>(dataset_name_, "dataset_name");
            bind_param<unsigned int>(frames_per_second_, "frames_per_second");
            bind_param<bool>(enable_text_overlay_, "enable_text_overlay");
            bind_param<bool>(enable_frame_number_overlay_, "enable_frame_number_overlay");
        }

        unsigned int frame_height_;               //!< Height of simulated frames in pixels
        unsigned int frame_width_;                //!< Width of simulated frames in pixels
        unsigned int frame_bitdepth_;             //!< Bit depth of simulated frames
        std::string image_file_path_;             //!< Path to HDF5 file containing test images
        std::string dataset_name_;                //!< Name of dataset within HDF5 file
        unsigned int frames_per_second_;          //!< Simulation frame rate
        bool enable_text_overlay_;                //!< Enable camera number text overlay
        bool enable_frame_number_overlay_;        //!< Enable frame number text overlay

        // Friend declarations for classes that need direct access to parameters
        friend class SimulatedDpdkCamera;
        friend class SimulatedImageGenerator;
    };
}

#endif // INCLUDE_SIMULATEDCAMERACONFIGURATION_H_