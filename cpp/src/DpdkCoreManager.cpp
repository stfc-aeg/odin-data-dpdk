#include "DpdkCoreManager.h"

#include <syslog.h>
#include <unistd.h>
#include <cstdio>
#include <string>
#include <algorithm>
#include <set>

#include <rte_memory.h>
#include <rte_launch.h>
#include <rte_eal.h>
#include <rte_per_lcore.h>
#include <rte_lcore.h>
#include <rte_debug.h>
#include <rte_errno.h>
#include <rte_ethdev.h>

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

        ParamContainer::Document config_params;
        config.copy_params(config_params);
        core_config_.update(config_params);

        // Redirect DPDK log output through log4cxx and suppress syslog during EAL init
        setlogmask(0x01);
        cookie_io_functions_t dpdk_log_funcs;
        std::memset(&dpdk_log_funcs, 0, sizeof(dpdk_log_funcs));
        dpdk_log_funcs.write = &DpdkCoreManager::dpdk_log_writer;

        FILE* org_stderr = stderr;
        stderr = fopencookie(nullptr, "w", dpdk_log_funcs);

        // Construct an argv list to pass to the DPDK EAL initialisation.
        // eal_strings owns the string data; eal_argv holds non-owning pointers into it.
        std::vector<std::string> eal_strings;
        std::vector<char *> eal_argv;
        int eal_argc = build_dpdk_eal_args(config, eal_strings, eal_argv);

        // Attempt to initialize the DPDK EAL - this may fail if already initialized
        int rc = rte_eal_init(eal_argc, eal_argv.data());
        if (rc < 0 && rte_errno != EALREADY)
        {
            std::stringstream ss;
            ss << "Failed to initialise DPDK EAL: " << rte_strerror(rte_errno);
            reply.set_msg_type(OdinData::IpcMessage::MsgTypeNack);
            reply.set_param("error", ss.str());
            throw std::runtime_error(ss.str());
        }
        else if (rc < 0 && rte_errno == EALREADY)
        {
            LOG4CXX_INFO(logger_, "DPDK EAL already initialized, continuing...");
        }

        setlogmask(0xff);
        fclose(stderr);
        stderr = org_stderr;

        rte_openlog_stream(fopencookie(nullptr, "w", dpdk_log_funcs));

        // Build the per-socket lcore availability map
        LOG4CXX_INFO(logger_, "Detected " << rte_socket_count() << " NUMA sockets");
        for (int i = 0; i < rte_socket_count(); i++)
        {
            available_core_ids_.push_back(std::vector<int>());
        }

        int lcore_id;
        LOG4CXX_INFO(logger_, "Mapping available DPDK worker lcores to sockets:");
        RTE_LCORE_FOREACH_WORKER(lcore_id)
        {
            unsigned int lcore_socket = rte_lcore_to_socket_id(lcore_id);
            LOG4CXX_INFO(logger_, "  DPDK lcore " << lcore_id << " -> socket " << lcore_socket);
            available_core_ids_[lcore_socket].push_back(lcore_id);
        }
        for (size_t i = 0; i < available_core_ids_.size(); i++)
        {
            std::stringstream ss;
            ss << "Socket " << i << " lcores: ";
            for (auto core : available_core_ids_[i]) ss << core << " ";
            LOG4CXX_INFO(logger_, ss.str());
        }


        if (!core_config_.worker_core_params_.IsObject())
        {
            LOG4CXX_WARN(logger_, "DpdkCoreManager: worker_core_params_ is not a valid object");
        }
        else
        {
            // Pass 1: Build core_chain_order_ bimap from each core's "connect" field
            for (auto itr = core_config_.worker_core_params_.MemberBegin();
                 itr != core_config_.worker_core_params_.MemberEnd(); ++itr)
            {
                const char* json_key = itr->name.GetString();
                const rapidjson::Value& core_cfg = itr->value;

                if (!core_cfg.IsObject()) continue;
                if (!core_cfg.HasMember("connect") || !core_cfg["connect"].IsString()) continue;

                std::string upstream_key = core_cfg["connect"].GetString();
                core_chain_order_.insert({upstream_key, json_key});
                LOG4CXX_DEBUG(logger_, "Core chain: " << upstream_key << " -> " << json_key);
            }

            // Pass 2: Inject num_downstream_cores into each upstream core and upstream_core
            //         into each downstream core so individual cores don't need these in config
            for (auto itr = core_config_.worker_core_params_.MemberBegin();
                 itr != core_config_.worker_core_params_.MemberEnd(); ++itr)
            {
                const char* json_key = itr->name.GetString();
                const rapidjson::Value& core_cfg = itr->value;

                if (!core_cfg.IsObject()) continue;

                if (core_cfg.HasMember("connect") && core_cfg["connect"].IsString() &&
                    core_cfg.HasMember("num_cores") && core_cfg["num_cores"].IsInt())
                {
                    std::string upstream_key = core_cfg["connect"].GetString();
                    int num_downstream = core_cfg["num_cores"].GetInt();

                    if (!core_config_.worker_core_params_.HasMember(upstream_key.c_str()))
                    {
                        LOG4CXX_ERROR(logger_, "DpdkCoreManager: upstream core '" << upstream_key << "' not found");
                        continue;
                    }

                    const rapidjson::Value& upstream_cfg =
                        core_config_.worker_core_params_[upstream_key.c_str()];

                    // secondary_fanout multiplies downstream count by (num_secondary_processes + 1)
                    if (upstream_cfg.IsObject() &&
                        upstream_cfg.HasMember("secondary_fanout") &&
                        upstream_cfg["secondary_fanout"].IsBool() &&
                        upstream_cfg["secondary_fanout"].GetBool())
                    {
                        num_downstream += num_downstream * core_config_.num_secondary_processes_;
                    }

                    core_config_.worker_core_params_[upstream_key.c_str()].AddMember(
                        "num_downstream_cores",
                        rapidjson::Value(num_downstream),
                        core_config_.worker_core_params_.GetAllocator()
                    );
                }

                // Inject upstream_core (the class name string, not the JSON key) into this core
                if (core_chain_order_.right.count(json_key) > 0)
                {
                    std::string upstream_key = core_chain_order_.right.at(json_key);
                    if (!core_config_.worker_core_params_.HasMember(upstream_key.c_str())) continue;

                    const rapidjson::Value& upstream_cfg =
                        core_config_.worker_core_params_[upstream_key.c_str()];

                    if (!upstream_cfg.HasMember("core_name") || !upstream_cfg["core_name"].IsString())
                        continue;

                    std::string upstream_class = upstream_cfg["core_name"].GetString();
                    core_config_.worker_core_params_[json_key].AddMember(
                        "upstream_core",
                        rapidjson::Value(upstream_class.c_str(), core_config_.worker_core_params_.GetAllocator()),
                        core_config_.worker_core_params_.GetAllocator()
                    );
                }
            }
        }

        // Hugepages buffer pool shared by all worker cores; first core to start creates
        // the clear_frames ring and populates it
        DpdkSharedBuffer* shared_buffer = new DpdkSharedBuffer(
            core_config_.shared_buffer_size_, decoder->get_frame_buffer_size(),
            core_config_.socket_
        );
        shared_buffers_.push_back(shared_buffer);

        LOG4CXX_INFO(logger_, "Created shared buffer:"
            << " socket " << core_config_.socket_
            << " total_size " << shared_buffer->get_mem_size()
            << " buffer_size " << shared_buffer->get_buffer_size()
            << " num_buffers " << shared_buffer->get_num_buffers()
        );

        DpdkWorkCoreReferences dpdkWorkCoreReferences = {
            core_config_,
            decoder,
            frame_callback_,
            shared_buffer,
        };

        if (core_config_.worker_core_params_.IsObject())
        {
            for (auto itr = core_config_.worker_core_params_.MemberBegin();
                 itr != core_config_.worker_core_params_.MemberEnd(); ++itr)
            {
                if (!itr->value.HasMember("num_cores") || !itr->value["num_cores"].IsInt()) continue;
                if (!itr->value.HasMember("core_name") || !itr->value["core_name"].IsString()) continue;

                unsigned int num_cores = itr->value["num_cores"].GetUint();
                std::string worker_class_name = itr->value["core_name"].GetString();
                unsigned int process_offset = num_cores * core_config_.dpdk_process_rank_;

                for (unsigned int i = 0; i < num_cores; i++)
                {
                    LOG4CXX_INFO(logger_, "Creating worker core: " << worker_class_name << " [" << (i + process_offset) << "]");
                    try
                    {
                        boost::shared_ptr<DpdkWorkerCore> core = FrameProcessor::DpdkCoreLoader<DpdkWorkerCore>::load_class(
                            worker_class_name.c_str(),
                            i + process_offset,
                            core_config_.socket_,
                            dpdkWorkCoreReferences
                        );
                        register_worker_core(core);
                    }
                    catch (const std::exception& e)
                    {
                        LOG4CXX_ERROR(logger_, "Failed to create worker core " << worker_class_name
                            << " [" << (i + process_offset) << "]: " << e.what()
                        );
                        throw;
                    }
                }
            }
        }
    }



    DpdkCoreManager::~DpdkCoreManager()
    {
        LOG4CXX_INFO(logger_, "Cleaning up DPDK core manager");

        stop();
        rte_delay_us_block(1000);

        for (auto& shared_buffer : shared_buffers_)
        {
            delete shared_buffer;
        }

        // Stop and close all active DPDK ethernet ports
        uint16_t port_id;
        for (port_id = 0; port_id < RTE_MAX_ETHPORTS; port_id++)
        {
            if (rte_eth_dev_is_valid_port(port_id))
            {
                rte_eth_dev_stop(port_id);
                rte_eth_dev_close(port_id);
            }
        }
    }

    void DpdkCoreManager::register_worker_core(boost::shared_ptr<DpdkWorkerCore> worker_core)
    {
        registered_cores_.push_back(worker_core);
    }

    bool DpdkCoreManager::start(void)
    {
        bool start_ok = true;

        LOG4CXX_INFO(logger_, "Current lcore: " << rte_lcore_id() << " socket: " << rte_socket_id());
        LOG4CXX_INFO(logger_, "Main lcore:    " << rte_get_main_lcore());

        for (boost::shared_ptr<DpdkWorkerCore>& core : registered_cores_)
        {
            core->connect();
        }

        int core_idx = 0;
        for (boost::shared_ptr<DpdkWorkerCore>& core : registered_cores_)
        {
            unsigned int core_socket = core->socket_id();
            int start_socket, end_socket;

            if (core_socket == SOCKET_ID_ANY)
            {
                start_socket = 0;
                end_socket = available_core_ids_.size();
            }
            else
            {
                start_socket = core_socket;
                end_socket = core_socket;
            }

            // Find first unused lcore on the requested socket
            int next_lcore_id = RTE_MAX_LCORE;
            for (int socket = start_socket; socket <= end_socket && next_lcore_id == RTE_MAX_LCORE; socket++)
            {
                for (auto& avail_id : available_core_ids_[socket])
                {
                    if (std::find(used_core_ids_.begin(), used_core_ids_.end(), avail_id) == used_core_ids_.end())
                    {
                        next_lcore_id = avail_id;
                        break;
                    }
                }
            }

            if (next_lcore_id == RTE_MAX_LCORE)
            {
                LOG4CXX_ERROR(logger_, "Error launching worker core " << core_idx << ": no lcores available on socket " << core_socket);
                start_ok = false;
                break;
            }

            LOG4CXX_INFO(logger_, "Launching worker core " << core_idx << " on lcore " << next_lcore_id);
            int launch_err = rte_eal_remote_launch(start_worker, core.get(), next_lcore_id);
            if (launch_err != 0)
            {
                LOG4CXX_ERROR(logger_, "Failed to launch worker on lcore " << next_lcore_id << " : " << strerror(launch_err));
                start_ok = false;
                break;
            }

            running_cores_.push_back(core);
            used_core_ids_.push_back(next_lcore_id);
            core_idx++;
        }

        if (!start_ok)
        {
            LOG4CXX_ERROR(logger_, "Core launch failed — stopping all cores that were started");
            stop();
        }

        return start_ok;
    }

    void DpdkCoreManager::stop(void)
    {
        if (running_cores_.empty())
        {
            LOG4CXX_WARN(logger_, "No running worker cores to stop");
        }

        for (boost::shared_ptr<DpdkWorkerCore>& core : running_cores_)
        {
            if (core)
            {
                uint32_t core_id = core->lcore_id();
                LOG4CXX_DEBUG(logger_, "Stopping worker on lcore " << core_id);
                core->stop();
                rte_eal_wait_lcore(core_id);
                used_core_ids_.erase(
                    std::remove(used_core_ids_.begin(), used_core_ids_.end(), core_id),
                    used_core_ids_.end()
                );
            }
        }

        rte_delay_us_block(1000);

        for (boost::shared_ptr<DpdkWorkerCore>& core : running_cores_)
        {
            if (core) core.reset();
        }

        if (!used_core_ids_.empty())
        {
            LOG4CXX_WARN(logger_, "used_core_ids_ still has " << used_core_ids_.size() << " entries after stop");
            used_core_ids_.clear();
        }

        running_cores_.clear();
        std::vector<boost::shared_ptr<DpdkWorkerCore>>(running_cores_).swap(running_cores_);
        registered_cores_.clear();
        std::vector<boost::shared_ptr<DpdkWorkerCore>>(registered_cores_).swap(registered_cores_);
    }

    ssize_t DpdkCoreManager::dpdk_log_writer(void *, const char *data, size_t len)
    {
        LoggerPtr logger = Logger::getLogger("FP.DpdkCoreManager");
        LOG4CXX_INFO(logger, "DPDK: " << std::string(data, len-1));
        return len;
    }

    int DpdkCoreManager::build_dpdk_eal_args(
        OdinData::IpcMessage& config,
        std::vector<std::string>& eal_strings,
        std::vector<char*>& eal_argv
    )
    {
        auto push = [&](std::string s) {
            eal_strings.push_back(std::move(s));
            eal_argv.push_back(const_cast<char*>(eal_strings.back().c_str()));
        };

        push("frameProcessor");

        if (config.has_param(DpdkCoreManager::CONFIG_DPDK_EAL_PARAMS))
        {
            if (dpdk_eal_param_map_.size() == 0)
            {
                dpdk_eal_param_map_["corelist"]   = "-l";
                dpdk_eal_param_map_["allow"]      = "--allow";
                dpdk_eal_param_map_["loglevel"]   = "--log-level";
                dpdk_eal_param_map_["allowdevice"]= "--allow";
                dpdk_eal_param_map_["proc-type"]  = "--proc-type";
                dpdk_eal_param_map_["file-prefix"]= "--file-prefix";
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
                        for (auto val_itr = itr->value.Begin(); val_itr != itr->value.End(); ++val_itr)
                        {
                            push(dpdk_eal_param_map_[param_name]);
                            push(param_value(*val_itr));
                        }
                    }
                    else
                    {
                        push(dpdk_eal_param_map_[param_name]);
                        push(param_value(itr->value));
                    }
                }
            }
        }
        eal_argv.push_back(NULL);

        return static_cast<int>(eal_argv.size()) - 1;
    }

    std::string DpdkCoreManager::param_value(const rapidjson::Value& param)
    {
        if (param.IsString())
        {
            return param.GetString();
        }
        rapidjson::StringBuffer buffer;
        rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
        param.Accept(writer);
        return buffer.GetString();
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

    void DpdkCoreManager::configure(OdinData::IpcMessage& config)
    {
        LOG4CXX_INFO(logger_, "DpdkCoreManager: Got update message: " << config.get_msg_val());

        for (boost::shared_ptr<DpdkWorkerCore>& core : registered_cores_)
        {
            core->configure(config);
        }
    }

    std::vector<std::pair<std::string, int>> DpdkCoreManager::requestCommands()
    {
        // Collect all commands across cores; for duplicates keep the highest priority
        std::map<std::string, int> best_priority;
        for (auto& core: registered_cores_)
        {
            for (auto& [cmd, priority]: core->requestCommands())
            {
                auto it = best_priority.find(cmd);
                if (it == best_priority.end() || priority > it->second)
                {
                    best_priority[cmd] = priority;
                }
            }
        }
        std::vector<std::pair<std::string, int>> all_commands(best_priority.begin(), best_priority.end());
        return all_commands;
    }

    void DpdkCoreManager::execute(const std::string& command, OdinData::IpcMessage& reply)
    {
        // Collect all cores that support this command, paired with their declared priority
        std::vector<std::pair<int, boost::shared_ptr<DpdkWorkerCore>>> candidates;
        for (auto& core: registered_cores_)
        {
            for (auto& [cmd, priority]: core->requestCommands())
            {
                if (cmd == command)
                {
                    candidates.emplace_back(priority, core);
                    break;
                }
            }
        }

        if (candidates.empty())
        {
            reply.set_nack("No core supports command: " + command);
            return;
        }

        // Sort descending by priority so highest runs first
        std::sort(candidates.begin(), candidates.end(),
            [](const auto& a, const auto& b) { return a.first > b.first; });

        for (auto& [priority, core]: candidates)
        {
            core->execute(command, reply);
        }
    }

}
