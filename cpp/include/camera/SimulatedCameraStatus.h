#ifndef INCLUDE_SIMULATEDCAMERASTATUS_H_
#define INCLUDE_SIMULATEDCAMERASTATUS_H_

#include "camera/DpdkCameraStatus.h"

namespace FrameProcessor
{
    /**
     * @class SimulatedCameraStatus
     * @brief Status container for simulated cameras
     *
     * This class extends the basic DPDK camera status with
     * additional parameters specific to simulated cameras.
     */
    class SimulatedCameraStatus : public DpdkCameraStatus
    {
    public:
        /**
         * @brief Default constructor
         *
         * Initializes all parameters to default values and binds them
         * in the parameter container.
         */
        SimulatedCameraStatus() :
            DpdkCameraStatus(),
            wibble_(false)
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
        SimulatedCameraStatus(const SimulatedCameraStatus& status) :
            DpdkCameraStatus(status)
        {
            // Bind the parameters in the container
            bind_params();

            // Update the container from the existing status object
            update(status);
        }

    private:
        /**
         * @brief Bind parameters in the container
         *
         * Overrides the base class method to bind both the common camera
         * status parameters and the simulation-specific parameters.
         */
        virtual void bind_params(void) override
        {
            // First bind base class parameters
            DpdkCameraStatus::bind_params();
            
            // Then bind the additional parameters
            bind_param<bool>(wibble_, "wibble");
        }

        bool wibble_;     //!< Example additional status parameter for simulated cameras
        
        // Friend classes that need direct access
        friend class SimulatedDpdkCamera;
    };
}

#endif // INCLUDE_SIMULATEDCAMERASTATUS_H_