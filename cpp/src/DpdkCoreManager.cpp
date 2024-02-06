/*
 * DpdkCoreManager.cpp -
 *
 * Created on: 04 July 2022
 *     Author: Tim Nicholls, STFC Detector Systems Software Group
 */

#include "DpdkCoreManager.h"

#include <syslog.h>
#include <unistd.h>
#include <cstdio>
#include <string>
#include <algorithm>

#include <rte_memory.h>
#include <rte_launch.h>
#include <rte_eal.h>
#include <rte_per_lcore.h>
#include <rte_lcore.h>
#include <rte_debug.h>
#include <rte_errno.h>

#include "DpdkUtils.h"
#include "DpdkCoreLoader.h"

using namespace OdinData;

namespace FrameProcessor
{

    const std::string DpdkCoreManager::CONFIG_DPDK_EAL_PARAMS = "dpdk_eal";

    DpdkCoreManager::DpdkCoreManager(
        OdinData::IpcMessage& config, OdinData::IpcMessage& reply,
        const std::string plugin_name, ProtocolDecoder* decoder, FrameCallback& frame_callback
    ) :
        logger_(Logger::getLogger("FP.DpdkCoreManager")),
        plugin_name_(plugin_name),
        frame_callback_(frame_callback)
    {
        LOG4CXX_INFO(logger_, "Initialising DPDK core manager");

        // Update core configuration parameters from the config message provided in the arguments
        ParamContainer::Document config_params;
        config.encode_params(config_params);
        core_config_.update(config_params);

        // Create a custom IO stream bound to a local method, which can be used to redirect DPDK
        // logging into the local logger instance. Also suppress syslog output and redirect stderr
        // during EAL initialisation.
        setlogmask(0x01);
        cookie_io_functions_t dpdk_log_funcs;
        std::memset(&dpdk_log_funcs, 0, sizeof(dpdk_log_funcs));
        dpdk_log_funcs.write = &DpdkCoreManager::dpdk_log_writer;

        FILE* org_stderr = stderr;
        stderr = fopencookie(nullptr, "w", dpdk_log_funcs);

        // Construct an argv list to pass to the DPDK EAL initialisation
        std::vector<char *> eal_argv;
        int eal_argc = build_dpdk_eal_args(config, eal_argv);

        // Initialise the DPDK EAL. This will pin the current thread of execution to the master
        // lcore
        int rc = rte_eal_init(eal_argc, eal_argv.data());
        if (rc < 0)
        {
            std::stringstream ss;
            ss << "Failed to initialise DPDK EAL: " << rte_strerror(rte_errno);
            // this->set_error(ss.str());
            reply.set_msg_type(OdinData::IpcMessage::MsgTypeNack);
            reply.set_param("error", ss.str());
        }

        // Restore syslog and stderr to their original state
        setlogmask(0xff);
        fclose(stderr);
        stderr = org_stderr;

        // Bind the custom IO stream to the DPDK logger
        rte_openlog_stream(fopencookie(nullptr, "w", dpdk_log_funcs));

       // Construct array of worker lcores by NUMA socket available to DPDK
        for (int i = 0 ; i < rte_socket_count(); i++)
        {
            std::vector<int> vc;
            available_core_ids_.push_back(vc);
        }
        int lcore_id;
        RTE_LCORE_FOREACH_WORKER(lcore_id)
        {
            unsigned int lcore_socket = rte_lcore_to_socket_id(lcore_id);
            available_core_ids_[lcore_socket].push_back(lcore_id);
        }


        if(core_config_.worker_core_params_.IsObject()) {

            // Construct the core_chain_order_
            for (rapidjson::Value::ConstMemberIterator itr = core_config_.worker_core_params_.MemberBegin();
                itr != core_config_.worker_core_params_.MemberEnd(); ++itr)
            {
                const char* json_key = itr->name.GetString();
                const rapidjson::Value& core_config = itr->value;

                if(core_config.HasMember("connect")) {
                    std::string upstream_core = core_config["connect"].GetString();

                    // Add this pair to the bimap
                    core_chain_order_.insert({upstream_core, json_key});
                }
            }

            // Iterate to add "num_downstream_cores" to the upstream cores
            for (rapidjson::Value::ConstMemberIterator itr = core_config_.worker_core_params_.MemberBegin();
                itr != core_config_.worker_core_params_.MemberEnd(); ++itr)
            {
                const char* json_key = itr->name.GetString();
                const rapidjson::Value& core_config = itr->value;

                if(core_config.HasMember("connect") && core_config["connect"].IsString()) {
                    std::string upstream_core_json_key = core_config["connect"].GetString();

                    // Find the core this one connects to, and add the new field "num_downstream_cores" to it
                    if(core_config.HasMember("num_cores") && core_config["num_cores"].IsInt()) {
                        int num_downstream_cores = core_config["num_cores"].GetInt();

                        // Check the upstream core for the "secondary_fanout" flag and adjust number of downstream cores accordingly
                        const rapidjson::Value& upstream_core_config = core_config_.worker_core_params_[upstream_core_json_key.c_str()];
                        if(upstream_core_config.HasMember("secondary_fanout") && upstream_core_config["secondary_fanout"].GetBool()) {
                            num_downstream_cores += num_downstream_cores * core_config_.num_secondary_processes_;
                        }
                                
                        // Ensure the upstream core exists before adding the member
                        if(core_config_.worker_core_params_.HasMember(upstream_core_json_key.c_str())) {
                            core_config_.worker_core_params_[upstream_core_json_key.c_str()].AddMember("num_downstream_cores", 
                                rapidjson::Value(num_downstream_cores), core_config_.worker_core_params_.GetAllocator());
                        }
                    }
                }

                // Find the "upstream_core" for the current core
                if(core_chain_order_.right.count(json_key) > 0)
                {
                    std::string upstream_core_name = core_chain_order_.right.at(json_key);
                    std::string upstream_core = core_config_.worker_core_params_[upstream_core_name.c_str()]["core_name"].GetString();

                    // Add the new field "upstream_core" to the current core.
                    core_config_.worker_core_params_[json_key].AddMember("upstream_core", 
                        rapidjson::Value(upstream_core.c_str(), core_config_.worker_core_params_.GetAllocator()), 
                        core_config_.worker_core_params_.GetAllocator());
                }
            }
        }

        



        // Loop over all the ethernet devices available to DPDK, creating a device for each of them
        int port_id;
        RTE_ETH_FOREACH_DEV(port_id)
        {
            DpdkDevice* device = new DpdkDevice(port_id);
            devices_.push_back(device);
            LOG4CXX_INFO(logger_, "Device on port " << port_id
                << " socket " << device->socket_id()
                << " has " << available_core_ids_[device->socket_id()].size()
                << " lcores available"
            );
        }

        // Loop over initialised devices, creating a shared memory buffer and registering worker
        // cores for each as appropriate
        for (auto& device: devices_)
        {
            
            // Create a shared buffer for packet processor cores to build raw frames into. This will
            // be shared between all PPCs, where the first to start will set up the frame processed
            // ring
            DpdkSharedBuffer* shared_buffer =
                new DpdkSharedBuffer(
                    core_config_.shared_buffer_size_, decoder->get_frame_buffer_size(),
                    device->socket_id()
                );
            shared_buffers_.push_back(shared_buffer);
            LOG4CXX_DEBUG(logger_, "Created shared buffer for device on port " << device->port_id()
                << " socket " << device->socket_id()
                << " total size " << shared_buffer->get_mem_size()
                << " buffer size " << shared_buffer->get_buffer_size()
                << " num buffers " << shared_buffer->get_num_buffers()
            );

            // Declare composite data structure to hold the configuration that all workers will likely require

            // change name
            DpdkWorkCoreReferences dpdkWorkCoreReferences = 
            {
                core_config_,
                decoder,
                frame_callback_,
                shared_buffer,
                device->port_id()

            };


            if(core_config_.worker_core_params_.IsObject()) {
                for (rapidjson::Value::ConstMemberIterator itr = core_config_.worker_core_params_.MemberBegin();
                    itr != core_config_.worker_core_params_.MemberEnd(); ++itr)
                {
                    // Check if the current worker has "num_cores" and "core_name"
                    if(itr->value.HasMember("num_cores") && itr->value["num_cores"].IsInt() &&
                    itr->value.HasMember("core_name") && itr->value["core_name"].IsString())
                    {
                        // Extract the number of cores and the worker class name
                        unsigned int num_cores = itr->value["num_cores"].GetUint();
                        std::string worker_class_name = itr->value["core_name"].GetString();

                        unsigned int process_offset = num_cores * core_config_.dpdk_process_rank_;

                        // Launch each worker core
                        for(unsigned int i = 0; i < num_cores; i++)
                        {
                            LOG4CXX_INFO(logger_, "Launching worker core from class: " << worker_class_name);

                            boost::shared_ptr<DpdkWorkerCore> core = FrameProcessor::DpdkCoreLoader<DpdkWorkerCore>::load_class(
                                worker_class_name.c_str(),
                                i + process_offset,
                                device->socket_id(),
                                dpdkWorkCoreReferences
                            );

                            register_worker_core(core);
                        }
                    }
                }
            }
        }
    }


    DpdkCoreManager::~DpdkCoreManager()
    {
        LOG4CXX_INFO(logger_, "Cleaning up DPDK core manager");

        // Stop all running worker cores and ethernet devices
        stop();

        // Delete shared buffers
        for (auto& shared_buffer: shared_buffers_)
        {
            delete shared_buffer;
        }

        // Delete devices
        for (auto& device: devices_)
        {
            delete device;
        }

        // Clean up the DPDK runtime environment
        rte_eal_cleanup();
    }

    void DpdkCoreManager::register_worker_core(boost::shared_ptr<DpdkWorkerCore> worker_core)
    {
        registered_cores_.push_back(worker_core);
    }

    bool DpdkCoreManager::start(void)
    {
        bool start_ok = true;

        LOG4CXX_INFO(logger_, "Current lcore: " << rte_lcore_id()
            <<" socket: " << rte_socket_id());
        LOG4CXX_INFO(logger_, "Main lcore:    " << rte_get_main_lcore());

        if (core_config_.dpdk_process_rank_ == 0)
        {
            // Start the ethernet devices
            for (auto& device: devices_)
            {
                device->start();
            }
        }

        // Connect all cores to their upstream resources

        for (boost::shared_ptr<DpdkWorkerCore>& core: registered_cores_)
        {
            core.get()->connect();
        }

        // Start all the registered worker cores
        int core_idx = 0;
        for (boost::shared_ptr<DpdkWorkerCore>& core: registered_cores_)
        {

            // Determine which, if any, socket the worker core should run on
            unsigned int core_socket = core->socket_id();
            int start_socket, end_socket = -1;

            if (core_socket == SOCKET_ID_ANY)
            {
                LOG4CXX_DEBUG_LEVEL(2, logger_, "Worker core " << core_idx
                    << " has not requested a specific socket"
                );
                start_socket = 0;
                end_socket = available_core_ids_.size();
            }
            else
            {
                LOG4CXX_DEBUG_LEVEL(2, logger_, "Worker core " << core_idx
                    << " wants socket id " << core_socket
                );
                start_socket = core_socket;
                end_socket = core_socket;
            }

            int next_lcore_id = RTE_MAX_LCORE;
            for (int socket = start_socket; socket <= end_socket; socket++)
            {
                for (auto& avail_id: available_core_ids_[socket])
                {
                    if (std::find(used_core_ids_.begin(), used_core_ids_.end(), avail_id) ==
                        std::end(used_core_ids_))
                    {
                        next_lcore_id = avail_id;
                        break;
                    }
                }
            }

            if (next_lcore_id == RTE_MAX_LCORE)
            {
                LOG4CXX_ERROR(logger_, "Error launching worker core " << core_idx
                    << ": no cores available on socket"
                );
                // TODO - undo other launches here?
                start_ok = false;
                break;
            }

            LOG4CXX_DEBUG(logger_, "Launching worker core " << core_idx
                << " on lcore "<< next_lcore_id
            );
            int launch_err = rte_eal_remote_launch(start_worker, core.get(), next_lcore_id);
            if (launch_err != 0)
            {
                LOG4CXX_ERROR(logger_,
                    "Failed to launch worker on lcore " << next_lcore_id <<
                    " : " << strerror(launch_err)
                );
                // TODO - undo other launches here?
                start_ok = false;
                break;
            }

            running_cores_.push_back(core);
            used_core_ids_.push_back(next_lcore_id);
            core_idx++;
        }
        return start_ok;
    }

    void DpdkCoreManager::stop(void)
    {
        // Warn if there are no running worker cores to stop
        if (running_cores_.empty())
        {
            LOG4CXX_WARN(logger_, "No running worker cores to stop");
        }

        // Stop all the running cores and clear the list of them
        for (boost::shared_ptr<DpdkWorkerCore>& core: running_cores_)
        {
            uint32_t core_id = core->lcore_id();
            LOG4CXX_DEBUG(logger_, "Stopping worker on lcore " << core_id);
            core->stop();
            rte_eal_wait_lcore(core_id);

            used_core_ids_.erase(
                std::remove(used_core_ids_.begin(), used_core_ids_.end(), core_id)
            );
            core.reset();
        }

        // The list of used core IDs should be empty now that all running cores have been stopped
        if (!used_core_ids_.empty())
        {
            LOG4CXX_WARN(logger_, "Stopped all running cores but used core ID list still contains"
                << used_core_ids_.size() << " cores"
            );
        }

        // Clear the list of running cores
        running_cores_.clear();
        std::vector<boost::shared_ptr<DpdkWorkerCore>>(running_cores_).swap(running_cores_);

        // Warn if there are no ethernet devices to stop
        if (devices_.empty())
        {
            LOG4CXX_WARN(logger_, "No devices to stop");
        }

        // Stop all the ethernet devices
        for (auto& device: devices_)
        {
            device->stop();
        }
    }

    ssize_t DpdkCoreManager::dpdk_log_writer(void *, const char *data, size_t len)
    {
        LoggerPtr logger = Logger::getLogger("FP.DpdkCoreManager");
        LOG4CXX_INFO(logger, "DPDK: " << std::string(data, len-1));
        return len;
    }

    int DpdkCoreManager::build_dpdk_eal_args(
        OdinData::IpcMessage& config, std::vector<char *>& eal_argv
    )
    {

        eal_argv.push_back(strdup("frameProcessor"));

        if (config.has_param(DpdkCoreManager::CONFIG_DPDK_EAL_PARAMS))
        {

            if (dpdk_eal_param_map_.size() == 0)
            {
                dpdk_eal_param_map_["corelist"] = "-l";
                dpdk_eal_param_map_["allow"] = "--allow";
                dpdk_eal_param_map_["loglevel"] = "--log-level";
                dpdk_eal_param_map_["allowdevice"] = "--allow";
                dpdk_eal_param_map_["proc-type"] = "--proc-type";
                dpdk_eal_param_map_["file-prefix"] = "--file-prefix";
            }

            const rapidjson::Value& eal_params =
            config.get_param<const rapidjson::Value&>(DpdkCoreManager::CONFIG_DPDK_EAL_PARAMS);

            for (rapidjson::Value::ConstMemberIterator itr = eal_params.MemberBegin();
                itr != eal_params.MemberEnd(); ++itr)
            {
                const char* param_name = itr->name.GetString();
                if (dpdk_eal_param_map_.count(param_name))
                {
                    if (itr->value.IsArray())
                    {
                        for (rapidjson::Value::ConstValueIterator val_itr = itr->value.Begin();
                                val_itr != itr->value.End(); ++val_itr)
                        {
                            eal_argv.push_back(strdup(dpdk_eal_param_map_[param_name]));
                            eal_argv.push_back(param_value(*val_itr));
                        }
                    }
                    else
                    {
                        eal_argv.push_back(strdup(dpdk_eal_param_map_[param_name]));
                        eal_argv.push_back(param_value(itr->value));
                    }
                }
            }
        }
        eal_argv.push_back(NULL);

        int eal_argc = eal_argv.size() - 1;

        return eal_argc;
    }

    char* DpdkCoreManager::param_value(const rapidjson::Value& param)
    {
        char* value;

        if (param.IsString())
        {
        value = strdup(param.GetString());
        }
        else
        {
            rapidjson::StringBuffer buffer;
            rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
            param.Accept(writer);
            value = strdup(buffer.GetString());
        }

        return value;
    }

    int DpdkCoreManager::start_worker(void* worker_ptr)
    {
        DpdkWorkerCore* worker_core = (DpdkWorkerCore*)worker_ptr;
        worker_core->run(rte_lcore_id());
        return 0;
    }

    // Definition of static member variables used for parameter mapping
    DpdkCoreManager::DpdkEalParamMap DpdkCoreManager::dpdk_eal_param_map_;

    void DpdkCoreManager::status(OdinData::IpcMessage& status)
    {
        LOG4CXX_DEBUG(logger_, "Status requested for core manager");

        std::string status_path = plugin_name_ + "/core_manager/";
        status.set_param(status_path + "shared_buffer_size", core_config_.shared_buffer_size_);

        // Loop through all running cores to and update their current status
        for (auto& core: running_cores_)
        {
            core->status(status, plugin_name_);
        }
    }


    




}