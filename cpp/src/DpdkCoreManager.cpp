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
            // Pass 1: Build core_chain_order_ (multimap) and downstream_to_upstream_ from "connect"
            for (auto itr = core_config_.worker_core_params_.MemberBegin();
                 itr != core_config_.worker_core_params_.MemberEnd(); ++itr)
            {
                const char* json_key = itr->name.GetString();
                const rapidjson::Value& core_cfg = itr->value;

                if (!core_cfg.IsObject()) continue;
                if (!core_cfg.HasMember("connect") || !core_cfg["connect"].IsString()) continue;

                std::string upstream_key = core_cfg["connect"].GetString();
                core_chain_order_.insert({upstream_key, json_key});
                downstream_to_upstream_[json_key] = upstream_key;
                LOG4CXX_DEBUG(logger_, "Core chain: " << upstream_key << " -> " << json_key);
            }

            // Pass 2: Inject derived metadata into each core's config block so individual
            //         cores don't need to re-derive it: num_downstream_cores, upstream_core
            //         (class name for ring lookups), config_key (JSON key for ring scoping),
            //         and stream_id.
            for (auto itr = core_config_.worker_core_params_.MemberBegin();
                 itr != core_config_.worker_core_params_.MemberEnd(); ++itr)
            {
                const char* json_key = itr->name.GetString();
                const rapidjson::Value& core_cfg = itr->value;

                if (!core_cfg.IsObject()) continue;

                // num_downstream_cores: sum of num_cores across all direct downstream entries
                {
                    int total_downstream = 0;
                    auto range = core_chain_order_.equal_range(json_key);
                    for (auto it = range.first; it != range.second; ++it)
                    {
                        const std::string& downstream_key = it->second;
                        if (!core_config_.worker_core_params_.HasMember(downstream_key.c_str())) continue;
                        const rapidjson::Value& ds_cfg =
                            core_config_.worker_core_params_[downstream_key.c_str()];
                        if (!ds_cfg.HasMember("num_cores") || !ds_cfg["num_cores"].IsInt()) continue;
                        int n = ds_cfg["num_cores"].GetInt();
                        // secondary_fanout multiplies count by (num_secondary_processes + 1)
                        if (ds_cfg.HasMember("secondary_fanout") &&
                            ds_cfg["secondary_fanout"].IsBool() &&
                            ds_cfg["secondary_fanout"].GetBool())
                        {
                            n += n * core_config_.num_secondary_processes_;
                        }
                        total_downstream += n;
                    }
                    if (total_downstream > 0)
                    {
                        core_config_.worker_core_params_[json_key].AddMember(
                            "num_downstream_cores",
                            rapidjson::Value(total_downstream),
                            core_config_.worker_core_params_.GetAllocator()
                        );
                        LOG4CXX_DEBUG(logger_, "Injected num_downstream_cores=" << total_downstream
                            << " into '" << json_key << "'");
                    }
                }

                // upstream_core: the config key of the upstream, used to look up ring names.
                //    Rings are now named after config keys (not C++ class names) to avoid
                //    collisions between streams running the same core class.
                {
                    auto upstream_it = downstream_to_upstream_.find(json_key);
                    if (upstream_it != downstream_to_upstream_.end())
                    {
                        const std::string& upstream_key = upstream_it->second;
                        core_config_.worker_core_params_[json_key].AddMember(
                            "upstream_core",
                            rapidjson::Value(upstream_key.c_str(),
                                core_config_.worker_core_params_.GetAllocator()),
                            core_config_.worker_core_params_.GetAllocator()
                        );
                        LOG4CXX_DEBUG(logger_, "Injected upstream_core='" << upstream_key
                            << "' into '" << json_key << "'");
                    }
                }

                // downstream_branches: array injected into cores that have direct downstream
                //    entries (e.g. packet_rx). Each element describes one downstream branch:
                //      { config_key, stream_id, num_cores, rx_ports[] }
                //    PacketRxCore uses this to set up per-stream ring groups and port routing.
                {
                    rapidjson::Document::AllocatorType& alloc =
                        core_config_.worker_core_params_.GetAllocator();
                    rapidjson::Value branches_arr(rapidjson::kArrayType);

                    auto range = core_chain_order_.equal_range(json_key);
                    for (auto it = range.first; it != range.second; ++it)
                    {
                        const std::string& ds_key = it->second;
                        if (!core_config_.worker_core_params_.HasMember(ds_key.c_str())) continue;
                        const rapidjson::Value& ds_cfg =
                            core_config_.worker_core_params_[ds_key.c_str()];

                        rapidjson::Value branch(rapidjson::kObjectType);

                        branch.AddMember("config_key",
                            rapidjson::Value(ds_key.c_str(), alloc), alloc);

                        std::string ds_stream_id = "";
                        if (ds_cfg.HasMember("stream") && ds_cfg["stream"].IsString())
                            ds_stream_id = ds_cfg["stream"].GetString();
                        branch.AddMember("stream_id",
                            rapidjson::Value(ds_stream_id.c_str(), alloc), alloc);

                        std::string ds_mode = "";
                        if (ds_cfg.HasMember("mode") && ds_cfg["mode"].IsString())
                            ds_mode = ds_cfg["mode"].GetString();
                        branch.AddMember("decoder_mode",
                            rapidjson::Value(ds_mode.c_str(), alloc), alloc);

                        int ds_num_cores = 0;
                        if (ds_cfg.HasMember("num_cores") && ds_cfg["num_cores"].IsInt())
                            ds_num_cores = ds_cfg["num_cores"].GetInt();
                        branch.AddMember("num_cores",
                            rapidjson::Value(ds_num_cores), alloc);

                        // Copy rx_ports from the downstream entry if present; falls back to
                        // an empty array (PacketRxCore treats empty as "accept all ports").
                        rapidjson::Value ports_arr(rapidjson::kArrayType);
                        if (ds_cfg.HasMember("rx_ports") && ds_cfg["rx_ports"].IsArray())
                        {
                            for (auto& p : ds_cfg["rx_ports"].GetArray())
                            {
                                ports_arr.PushBack(rapidjson::Value(p.GetInt()), alloc);
                            }
                        }
                        branch.AddMember("rx_ports", ports_arr, alloc);

                        branches_arr.PushBack(branch, alloc);
                        LOG4CXX_DEBUG(logger_, "Branch entry for '" << json_key
                            << "': ds_key='" << ds_key
                            << "' stream='" << ds_stream_id
                            << "' num_cores=" << ds_num_cores);
                    }

                    if (!branches_arr.Empty())
                    {
                        core_config_.worker_core_params_[json_key].AddMember(
                            "downstream_branches", branches_arr, alloc);
                        LOG4CXX_DEBUG(logger_, "Injected downstream_branches into '" << json_key << "'");
                    }
                }

                // config_key: the JSON key for this entry, used to scope ring names per stream
                core_config_.worker_core_params_[json_key].AddMember(
                    "config_key",
                    rapidjson::Value(json_key, core_config_.worker_core_params_.GetAllocator()),
                    core_config_.worker_core_params_.GetAllocator()
                );

                // stream_id: injected so it's visible in the config block (matches "stream" field
                //    or empty string for cores with no stream assignment)
                if (!core_cfg.HasMember("stream"))
                {
                    core_config_.worker_core_params_[json_key].AddMember(
                        "stream_id",
                        rapidjson::Value("", core_config_.worker_core_params_.GetAllocator()),
                        core_config_.worker_core_params_.GetAllocator()
                    );
                }
                else
                {
                    // Already present as "stream"; mirror it as "stream_id" for consistency
                    core_config_.worker_core_params_[json_key].AddMember(
                        "stream_id",
                        rapidjson::Value(core_cfg["stream"].GetString(),
                            core_config_.worker_core_params_.GetAllocator()),
                        core_config_.worker_core_params_.GetAllocator()
                    );
                }
            }

            // Pass 3: Collect unique stream IDs and create one DpdkSharedBuffer per stream.
            //         Cores with no "stream" field fall into the default stream "".
            // Build a map of stream_id -> mode string from the first core entry per stream
            std::map<std::string, std::string> stream_modes;
            for (auto itr = core_config_.worker_core_params_.MemberBegin();
                 itr != core_config_.worker_core_params_.MemberEnd(); ++itr)
            {
                const rapidjson::Value& core_cfg = itr->value;
                if (!core_cfg.IsObject()) continue;
                std::string sid = core_cfg.HasMember("stream") && core_cfg["stream"].IsString()
                    ? core_cfg["stream"].GetString() : "";
                if (stream_modes.find(sid) == stream_modes.end())
                {
                    std::string mode = core_cfg.HasMember("mode") && core_cfg["mode"].IsString()
                        ? core_cfg["mode"].GetString() : "";
                    stream_modes[sid] = mode;
                }
            }

            std::set<std::string> seen_streams;
            for (auto itr = core_config_.worker_core_params_.MemberBegin();
                 itr != core_config_.worker_core_params_.MemberEnd(); ++itr)
            {
                const rapidjson::Value& core_cfg = itr->value;
                if (!core_cfg.IsObject()) continue;

                std::string stream_id = "";
                if (core_cfg.HasMember("stream") && core_cfg["stream"].IsString())
                    stream_id = core_cfg["stream"].GetString();

                if (!stream_id.empty() && seen_streams.find(stream_id) == seen_streams.end())
                {
                    seen_streams.insert(stream_id);
                    const std::string& stream_mode = stream_modes.count(stream_id)
                        ? stream_modes.at(stream_id) : "";
                    DpdkSharedBuffer* buf = new DpdkSharedBuffer(
                        core_config_.shared_buffer_size_,
                        decoder->get_frame_buffer_size(stream_mode),
                        core_config_.socket_,
                        stream_id
                    );
                    stream_shared_buffers_[stream_id] = buf;
                    shared_buffers_.push_back(buf);
                    LOG4CXX_INFO(logger_, "Created shared buffer for stream '" << stream_id << "':"
                        << " socket " << core_config_.socket_
                        << " total_size " << buf->get_mem_size()
                        << " buffer_size " << buf->get_buffer_size()
                        << " num_buffers " << buf->get_num_buffers()
                    );
                }
            }
        }

        // Pass 4: Instantiate each worker core, injecting the stream-specific shared buffer,
        //         decoder mode, and config key via DpdkWorkCoreReferences.
        if (core_config_.worker_core_params_.IsObject())
        {
            for (auto itr = core_config_.worker_core_params_.MemberBegin();
                 itr != core_config_.worker_core_params_.MemberEnd(); ++itr)
            {
                const char* json_key = itr->name.GetString();
                const rapidjson::Value& core_cfg = itr->value;

                if (!core_cfg.HasMember("num_cores") || !core_cfg["num_cores"].IsInt()) continue;
                if (!core_cfg.HasMember("core_name") || !core_cfg["core_name"].IsString()) continue;

                unsigned int num_cores = core_cfg["num_cores"].GetUint();
                std::string worker_class_name = core_cfg["core_name"].GetString();
                unsigned int process_offset = num_cores * core_config_.dpdk_process_rank_;

                std::string stream_id = "";
                if (core_cfg.HasMember("stream") && core_cfg["stream"].IsString())
                    stream_id = core_cfg["stream"].GetString();

                std::string decoder_mode = "";
                if (core_cfg.HasMember("mode") && core_cfg["mode"].IsString())
                    decoder_mode = core_cfg["mode"].GetString();

                DpdkSharedBuffer* stream_buf = stream_shared_buffers_.count(stream_id)
                    ? stream_shared_buffers_.at(stream_id)
                    : (shared_buffers_.empty() ? nullptr : shared_buffers_.front());

                DpdkWorkCoreReferences refs = {
                    core_config_,
                    decoder,
                    decoder_mode,
                    stream_id,
                    std::string(json_key),
                    frame_callback_,
                    stream_buf,
                };

                for (unsigned int i = 0; i < num_cores; i++)
                {
                    LOG4CXX_INFO(logger_, "Creating worker core: " << worker_class_name
                        << " [" << (i + process_offset) << "]"
                        << " stream='" << stream_id << "'"
                        << " mode='" << decoder_mode << "'");
                    try
                    {
                        boost::shared_ptr<DpdkWorkerCore> core =
                            FrameProcessor::DpdkCoreLoader<DpdkWorkerCore>::load_class(
                                worker_class_name,
                                i + process_offset,
                                core_config_.socket_,
                                refs
                            );
                        register_worker_core(core);
                    }
                    catch (const std::exception& e)
                    {
                        LOG4CXX_ERROR(logger_, "Failed to create worker core " << worker_class_name
                            << " [" << (i + process_offset) << "]: " << e.what());
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

            LOG4CXX_INFO(logger_, "Launching worker core " << core_idx << " on lcore "
                << next_lcore_id << " (socket " << rte_lcore_to_socket_id(next_lcore_id) << ")");
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
        // eal_argv is populated with pointers into eal_strings only after all strings have
        // been collected, since eal_strings.push_back() may reallocate its buffer and
        // invalidate any c_str() pointers taken before the vector stopped growing.
        auto push = [&](std::string s) {
            eal_strings.push_back(std::move(s));
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

        // All strings are now finalised, so eal_strings will not reallocate again -
        // safe to take stable c_str() pointers into eal_argv.
        for (std::string& s : eal_strings)
        {
            eal_argv.push_back(const_cast<char*>(s.c_str()));
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

    void DpdkCoreManager::requestConfiguration(OdinData::IpcMessage& reply)
    {
    // Return the configuration of the plugin
    LOG4CXX_TRACE(logger_, "Configuration requested for DpdkCoreManager plugin");
        for (boost::shared_ptr<DpdkWorkerCore>& core: registered_cores_)
        {
            core.get()->requestConfiguration(reply);
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
            LOG4CXX_DEBUG(logger_, "Executing command '" << command
                << "' on core " << core->lcore_id());
            core->execute(command, reply);
        }
    }

}
