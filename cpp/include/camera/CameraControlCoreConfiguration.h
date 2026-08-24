#include "ParamContainer.h"
#include "DpdkCoreConfiguration.h"
#include <sstream>

namespace FrameProcessor
{
    namespace Defaults
    {
        const std::string CAMERA_CONTROL_ZMQ_ADDRESS = "tcp://127.0.0.1:9001";
    }


    class CameraControlCoreConfiguration : public OdinData::ParamContainer
    {

        public:

            CameraControlCoreConfiguration() :
                ParamContainer(),
                zmq_address_(Defaults::CAMERA_CONTROL_ZMQ_ADDRESS)
            {
                bind_params();
            }

            void resolve(DpdkCoreConfiguration& core_config_)
            {
                const ParamContainer::Value* value_ptr =
                    core_config_.get_worker_core_config("camera_control");

                if (value_ptr != nullptr)
                {
                    update(*value_ptr);
                }        
            }

        protected:

            virtual void bind_params(void)
            {
                bind_param<std::string>(core_name, "core_name");
                bind_param<std::string>(connect, "connect");
                bind_param<unsigned int>(num_cores, "num_cores");
                bind_param<std::string>(upstream_core, "upstream_core");
                bind_param<unsigned int>(num_downstream_cores, "num_downstream_cores");
                bind_param<unsigned int>(frame_timeout_, "frame_timeout");
                bind_param<double>(exposure_time_, "exposure_time");
                bind_param<double>(frame_rate_, "frame_rate");
                bind_param<std::string>(zmq_address_, "zmq_address");
            }

            
            std::string core_name;
            std::string connect;
            std::string upstream_core;
            unsigned int num_cores;
            unsigned int num_downstream_cores;
            unsigned int frame_timeout_;
            double exposure_time_;        //!< Exposure time in seconds
            double frame_rate_;           //!< Frame rate in Hertz
            std::string zmq_address_;     //!< ZMQ ROUTER bind address for the camera control IPC channel



            friend class CameraControlCore;
    };
}