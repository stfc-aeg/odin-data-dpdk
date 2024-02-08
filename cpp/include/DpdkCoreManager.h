/*
 * DpdkCoreManager.h
 *
 * Created on: 04 July 2022
 *     Author: Tim Nicholls, STFC Detector Systems Software Group
 */

#ifndef INCLUDE_DPDKCORE_MANAGER_H_
#define INCLUDE_DPDKCORE_MANAGER_H_

#include <boost/bimap.hpp>
#include <vector>
#include <string>
#include <unistd.h>
#include <map>
#include <iostream>
#include <log4cxx/logger.h>
using namespace log4cxx;
using namespace log4cxx::helpers;
#include <DebugLevelLogger.h>
#include <IpcMessage.h>
#include "DpdkDevice.h"
#include "DpdkWorkerCore.h"

#include "DpdkSharedBuffer.h"
#include "DpdkCoreConfiguration.h"
#include "ProtocolDecoder.h"
namespace FrameProcessor
{

    class DpdkCoreManager
    {

    public:
        DpdkCoreManager(
            OdinData::IpcMessage& config, OdinData::IpcMessage& reply,
            const std::string plugin_name, ProtocolDecoder* decoder, FrameCallback& frame_callback
        );
        ~DpdkCoreManager();

        void status(OdinData::IpcMessage& status);
        void register_worker_core(boost::shared_ptr<DpdkWorkerCore> worker_core);
        bool start(void);
        void stop(void);
        void configure(OdinData::IpcMessage& config);

    private:

        // DpdkCoreFactory dpdkCoreFactory;

        static const std::string CONFIG_DPDK_EAL_PARAMS;

        static ssize_t dpdk_log_writer(void *, const char *data, size_t len);
        int build_dpdk_eal_args(OdinData::IpcMessage& config, std::vector<char *>& eal_argv);
        char* param_value(const rapidjson::Value& param);

        static int start_worker(void* worker_ptr);

        boost::bimap<std::string, std::string> core_chain_order_;

        LoggerPtr logger_;

        std::string plugin_name_;
        FrameCallback frame_callback_;

        DpdkCoreConfiguration core_config_;

        // Internal map to associate DPDK EAL parameters with arguments
        typedef std::map<std::string, const char*> DpdkEalParamMap;
        static DpdkEalParamMap dpdk_eal_param_map_;

        std::vector<DpdkDevice *> devices_;
        std::vector<std::vector<int> > available_core_ids_;
        std::vector<int> used_core_ids_;
        std::vector<boost::shared_ptr<DpdkWorkerCore>> registered_cores_;
        std::vector<boost::shared_ptr<DpdkWorkerCore>> running_cores_;

        std::vector<DpdkSharedBuffer *> shared_buffers_;

    };
}
#endif /* INCLUDE_DPDKCORE_MANAGER_H_ */
