#ifndef DPDKCORECONFIGURATION_H_
#define DPDKCORECONFIGURATION_H_

#include "ParamContainer.h"

namespace FrameProcessor
{

    namespace Defaults
    {
        const std::size_t default_shared_buffer_size = 8589934592;
        const unsigned int default_num_processor_cores = 3;
        const unsigned int default_num_framebuilder_cores = 4;
        const unsigned int default_num_framecompression_cores = 0;
        const bool default_enable_compression = false;
        const std::string default_dataset_name = "dummy";
        const unsigned int default_num_secondary_processes = 0;
        const unsigned int default_dpdk_process_rank = 0;
        const unsigned int default_compression_enable = 1;
        const unsigned int default_blosc_clevel = 4;
        const unsigned int default_blosc_doshuffle = 2;
        const unsigned int default_blosc_compcode = 1;
        const unsigned int default_blosc_blocksize = 0;
        const unsigned int default_blosc_num_threads = 1;
    }

    class DpdkCoreConfiguration : public OdinData::ParamContainer
    {
        public:

            //! Default constructor
            //!
            //! This constructor initialises all parameters to default values and binds the
            //! parameters in the container, allowing them to accessed via the path-like set/get
            //! mechanism implemented in ParamContainer.

            DpdkCoreConfiguration() :
                ParamContainer(),
                shared_buffer_size_(Defaults::default_shared_buffer_size),
                num_processor_cores_(Defaults::default_num_processor_cores),
                num_framebuilder_cores_(Defaults::default_num_framebuilder_cores),
                num_framecompression_cores_(Defaults::default_num_framecompression_cores),
                enable_compression_(Defaults::default_enable_compression),
                num_secondary_processes_(Defaults::default_num_secondary_processes),
                dpdk_process_rank_(Defaults::default_dpdk_process_rank)
            {
                bind_params();
            }

            //! Copy constructor
            //!
            //! This constructor creates a copy of an existing configuration object. All parameters
            //! are first bound and then the underlying parameter container is updated from the
            //! existing object. This mechansim is necessary (rather than relying on a default copy
            //! constructor) since it is not possible for a parameter container to automatically
            //! rebind parameters defined in a derived class.
            //!
            //! \param config - existing config object to copy

            DpdkCoreConfiguration(const DpdkCoreConfiguration& config) :
                ParamContainer(config)
            {
                // Bind the parameters in the container
                bind_params();

                // Update the container from the existing config object
                update(config);
            }

            const ParamContainer::Value* get_packet_rx_config(const unsigned int core_idx)
            {
                std::stringstream ptr_ss;
                ptr_ss <<  "/" << core_idx;

                const ParamContainer::Value* value_ptr =
                    rapidjson::Pointer(ptr_ss.str().c_str()).Get(packet_rx_params_);
                return value_ptr;
            }

            const ParamContainer::Value* get_packet_processor_config(const unsigned int core_idx)
            {
                std::stringstream ptr_ss;
                ptr_ss <<  "/" << core_idx;

                const ParamContainer::Value* value_ptr =
                    rapidjson::Pointer(ptr_ss.str().c_str()).Get(packet_processor_params_);
                return value_ptr;
            }

            const ParamContainer::Value* get_frame_builder_config(const unsigned int core_idx)
            {
                std::stringstream ptr_ss;
                ptr_ss <<  "/" << core_idx;

                const ParamContainer::Value* value_ptr =
                    rapidjson::Pointer(ptr_ss.str().c_str()).Get(frame_builder_params_);
                return value_ptr;
            }

            const ParamContainer::Value* get_frame_compressor_config(const unsigned int core_idx)
            {
                std::stringstream ptr_ss;
                ptr_ss <<  "/" << core_idx;

                const ParamContainer::Value* value_ptr =
                    rapidjson::Pointer(ptr_ss.str().c_str()).Get(frame_compressor_params_);
                return value_ptr;
            }

            const ParamContainer::Value* get_frame_wrapper_config(const unsigned int core_idx)
            {
                std::stringstream ptr_ss;
                ptr_ss <<  "/" << core_idx;

                const ParamContainer::Value* value_ptr =
                    rapidjson::Pointer(ptr_ss.str().c_str()).Get(frame_wrapper_params_);
                return value_ptr;
            }

            const ParamContainer::Value* get_worker_core_config(const std::string& core_name)
            {
                std::string ptr_path = "/" + core_name;

                const ParamContainer::Value* value_ptr =
                    rapidjson::Pointer(ptr_path.c_str()).Get(worker_core_params_);
                return value_ptr;
            }

        private:

            //! Bind parameters in the container
            //!
            //! This method binds all the parameters in the container to named paths. This method
            //! is separated out so that it can be used in both default and copy constructors.

            virtual void bind_params(void)
            {
                bind_param<std::size_t>(shared_buffer_size_, "shared_buffer_size");
                bind_param<unsigned int>(num_processor_cores_, "num_processor_cores");
                bind_param<unsigned int>(num_framebuilder_cores_, "num_framebuilder_cores");
                bind_param<unsigned int>(num_framecompression_cores_, "num_framecompression_cores");
                bind_param<unsigned int>(num_secondary_processes_, "num_secondary_processes");
                bind_param<unsigned int>(dpdk_process_rank_, "dpdk_process_rank");
                bind_param<bool>(enable_compression_, "enable_compression");
                bind_param<ParamContainer::Document>(packet_rx_params_, "packet_rx");
                bind_param<ParamContainer::Document>(packet_processor_params_, "packet_processor");
                bind_param<ParamContainer::Document>(frame_builder_params_, "frame_builder");
                bind_param<ParamContainer::Document>(frame_compressor_params_, "frame_compressor");
                bind_param<ParamContainer::Document>(frame_wrapper_params_, "frame_wrapper");
                bind_param<ParamContainer::Document>(worker_core_params_, "worker_cores");
            }

            std::size_t shared_buffer_size_;      //!< DPDK memzone shared buffer size
            unsigned int num_processor_cores_;    //!< Number of packet processor cores to run
            unsigned int num_framebuilder_cores_; //!< Number of frame builder cores to run
            unsigned int num_framecompression_cores_; //!< Number of frame compression cores to run
            unsigned int num_secondary_processes_;
            unsigned int dpdk_process_rank_;

            bool enable_compression_; //!< Enable the compression cores
            ParamContainer::Document packet_rx_params_;
            ParamContainer::Document packet_processor_params_;
            ParamContainer::Document frame_builder_params_;
            ParamContainer::Document frame_compressor_params_;
            ParamContainer::Document frame_wrapper_params_;
            ParamContainer::Document worker_core_params_;


        //! Allow the DpdkCoreManager class direct access to config parameters
        friend class DpdkCoreManager;
        friend class PacketRxConfiguration;
        friend class PacketProcessorConfiguration;
        friend class FrameBuilderConfiguration;
        friend class FrameCompressorConfiguration;
        friend class FrameWrapperConfiguration;
    };
}

#endif // DPDKCORECONFIGURATION_H_