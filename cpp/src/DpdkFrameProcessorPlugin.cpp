#include "DpdkFrameProcessorPlugin.h"
#include "version.h"

namespace FrameProcessor
{
      /**
   * The constructor sets up logging used within the class.
   */
  DpdkFrameProcessorPlugin::DpdkFrameProcessorPlugin()
  {
    // Setup logging for the class
    logger_ = Logger::getLogger("FP.DpdkFrameProcessorPlugin");
  }

  /**
   * Destructor.
   */
  DpdkFrameProcessorPlugin::~DpdkFrameProcessorPlugin()
  {
    LOG4CXX_TRACE(logger_, "DpdkFrameProcessorPlugin destructor.");
  }

  /**
   * Get the plugin major version number.
   *
   * \return major version number as an integer
   */
  int DpdkFrameProcessorPlugin::get_version_major()
  {
    return ODINDATA_DPDK_VERSION_MAJOR;
  }

  /**
   * Get the plugin minor version number.
   *
   * \return minor version number as an integer
   */
  int DpdkFrameProcessorPlugin::get_version_minor()
  {
    return ODINDATA_DPDK_VERSION_MINOR;
  }

  /**
   * Get the plugin patch version number.
   *
   * \return patch version number as an integer
   */
  int DpdkFrameProcessorPlugin::get_version_patch()
  {
    return ODINDATA_DPDK_VERSION_PATCH;
  }

  /**
   * Get the plugin short version (e.g. x.y.z) string.
   *
   * \return short version as a string
   */
  std::string DpdkFrameProcessorPlugin::get_version_short()
  {
    return ODINDATA_DPDK_VERSION_STR_SHORT;
  }

  /**
   * Get the plugin long version (e.g. x.y.z-qualifier) string.
   *
   * \return long version as a string
   */
  std::string DpdkFrameProcessorPlugin::get_version_long()
  {
    return ODINDATA_DPDK_VERSION_STR;
  }

  /**
   * Configure the plugin.  This receives an IpcMessage which should be processed
   * to configure the plugin, and any response can be added to the reply IpcMessage.
   *
   * \param[in] config - Reference to the configuration IpcMessage object.
   * \param[out] reply - Reference to the reply IpcMessage object.
   */
  void DpdkFrameProcessorPlugin::configure(
    OdinData::IpcMessage& config, OdinData::IpcMessage& reply,
    ProtocolDecoder* decoder_ptr, FrameCallback& frame_callback)
  {
    LOG4CXX_INFO(logger_, "Configuring DPDKFrameProcessor plugin");

    if (config.get_param("update_config", false))
    {
      LOG4CXX_INFO(logger_, "Got update config");
      if (core_manager_ != nullptr)
      {
        core_manager_->configure(config);
      }
    }
    else
    {
      core_manager_.reset(
        new DpdkCoreManager(config, reply, this->get_name(), decoder_ptr, frame_callback)
      );
      core_manager_->start();
    }

  }

  void DpdkFrameProcessorPlugin::requestConfiguration(OdinData::IpcMessage& reply)
  {
    // Return the configuration of the plugin
    LOG4CXX_DEBUG(logger_, "Configuration requested for DPDKFrameProcessor plugin");
  }

  /**
   * Collate status information for the plugin.  The status is added to the status IpcMessage object.
   *
   * \param[out] status - Reference to an IpcMessage value to store the status.
   */
  void DpdkFrameProcessorPlugin::status(OdinData::IpcMessage& status)
  {
    // Record the plugin's status items
    LOG4CXX_DEBUG(logger_, "Status requested for DPDKFrameProcessor plugin");

    core_manager_->status(status);
  }

  /**
   * Reset process plugin statistics, i.e. counter of packets lost
   */
  bool DpdkFrameProcessorPlugin::reset_statistics(void)
  {
    LOG4CXX_DEBUG(logger_, "Statistics reset requested for DPDKFrameProcessor plugin")

    return true;
  }

}