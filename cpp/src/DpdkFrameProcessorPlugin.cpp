#include "DpdkFrameProcessorPlugin.h"
#include "version.h"

namespace FrameProcessor
{
  DpdkFrameProcessorPlugin::DpdkFrameProcessorPlugin()
  {
    logger_ = Logger::getLogger("FP.DpdkFrameProcessorPlugin");
  }

  DpdkFrameProcessorPlugin::~DpdkFrameProcessorPlugin()
  {
    LOG4CXX_TRACE(logger_, "DpdkFrameProcessorPlugin destructor.");
  }

  int DpdkFrameProcessorPlugin::get_version_major() { return ODINDATA_DPDK_VERSION_MAJOR; }
  int DpdkFrameProcessorPlugin::get_version_minor() { return ODINDATA_DPDK_VERSION_MINOR; }
  int DpdkFrameProcessorPlugin::get_version_patch() { return ODINDATA_DPDK_VERSION_PATCH; }
  std::string DpdkFrameProcessorPlugin::get_version_short() { return ODINDATA_DPDK_VERSION_STR_SHORT; }
  std::string DpdkFrameProcessorPlugin::get_version_long() { return ODINDATA_DPDK_VERSION_STR; }

  void DpdkFrameProcessorPlugin::configure(
    OdinData::IpcMessage& config, OdinData::IpcMessage& reply,
    ProtocolDecoder* decoder_ptr, FrameCallback& frame_callback)
  {
    if (config.get_param("update_config", false))
    {
      if (core_manager_ != nullptr)
      {
        core_manager_->configure(config);
      }
    }
    else
    {
      if (core_manager_)
      {
        core_manager_->stop();
        core_manager_.reset();
      }
      core_manager_.reset(new DpdkCoreManager(config, reply, this->get_name(), decoder_ptr, frame_callback));
      core_manager_->start();
    }
  }

  void DpdkFrameProcessorPlugin::requestConfiguration(OdinData::IpcMessage& reply)
  {
    LOG4CXX_TRACE(logger_, "Configuration requested for DPDKFrameProcessor plugin");
  }

  void DpdkFrameProcessorPlugin::status(OdinData::IpcMessage& status)
  {
    if (core_manager_ != nullptr)
    {
      core_manager_->status(status);
    }
  }

  void DpdkFrameProcessorPlugin::execute(const std::string& command, OdinData::IpcMessage& reply)
  {
    if (core_manager_ != nullptr)
    {
      core_manager_->execute(command, reply);
    }
    else
    {
      reply.set_nack("DPDK core manager not initialised");
    }
  }

  std::vector<std::string> DpdkFrameProcessorPlugin::requestCommands()
  {
    if (core_manager_ != nullptr)
    {
      std::vector<std::string> commands;
      for (auto& [cmd, priority] : core_manager_->requestCommands())
      {
        commands.push_back(cmd);
      }
      return commands;
    }
    return {};
  }

  bool DpdkFrameProcessorPlugin::reset_statistics(void)
  {
    return true;
  }
}