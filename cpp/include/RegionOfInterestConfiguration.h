#include "ParamContainer.h"
#include "DpdkCoreConfiguration.h"
#include <sstream>

namespace FrameProcessor
{
    class RegionOfInterestConfiguration : public OdinData::ParamContainer
    {

        public:

            RegionOfInterestConfiguration() :
                ParamContainer(),
                num_cores(0),
                num_downstream_cores(0),
                roi_x0_(0),
                roi_y0_(0),
                roi_x1_(0),
                roi_y1_(0)
            {
                bind_params();
            }

            void resolve(DpdkCoreConfiguration& core_config_, const std::string& config_key = "region_of_interest")
            {
                const ParamContainer::Value* value_ptr =
                    core_config_.get_worker_core_config(config_key);

                if (value_ptr != nullptr)
                {
                    update(*value_ptr);
                }
            }

        private:

            virtual void bind_params(void)
            {
                bind_param<std::string>(core_name, "core_name");
                bind_param<std::string>(connect, "connect");
                bind_param<std::string>(upstream_core, "upstream_core");
                bind_param<std::string>(config_key, "config_key");
                bind_param<std::string>(stream_id, "stream_id");
                bind_param<std::string>(decoder_mode, "mode");
                bind_param<unsigned int>(num_cores, "num_cores");
                bind_param<unsigned int>(num_downstream_cores, "num_downstream_cores");

                // Region of interest, specified as the top-left (x0, y0) and
                // bottom-right (x1, y1) coordinates of the crop box, in pixels
                bind_param<unsigned int>(roi_x0_, "roi_x0");
                bind_param<unsigned int>(roi_y0_, "roi_y0");
                bind_param<unsigned int>(roi_x1_, "roi_x1");
                bind_param<unsigned int>(roi_y1_, "roi_y1");
            }

            std::string core_name;
            std::string connect;
            std::string upstream_core;
            std::string config_key;
            std::string stream_id;
            std::string decoder_mode;
            unsigned int num_cores;
            unsigned int num_downstream_cores;

            unsigned int roi_x0_;
            unsigned int roi_y0_;
            unsigned int roi_x1_;
            unsigned int roi_y1_;

            friend class RegionOfInterestCore;
    };
}
