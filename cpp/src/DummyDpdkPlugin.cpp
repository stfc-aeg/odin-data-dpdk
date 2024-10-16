/*
 * DummyDpdkPlugin.cpp
 *
 *  Created on: 18 September 2024
 *      Author: Dominic Banks, STFC Detector Systems Software Group
 */

#include "DummyDpdkPlugin.h"
#include "version.h"

namespace FrameProcessor
{

  /**
   * The constructor sets up logging used within the class.
   */
  DummyDpdkPlugin::DummyDpdkPlugin() :
    DpdkFrameProcessorPlugin()
  {
    // Setup logging for the class
    logger_ = Logger::getLogger("FP.DummyDpdkPlugin");
    logger_->setLevel(Level::getAll());
    LOG4CXX_INFO(logger_, "DummyDpdkPlugin version " << this->get_version_long() << " loaded");

  }

  /**
   * Destructor.
   */
  DummyDpdkPlugin::~DummyDpdkPlugin()
  {
    LOG4CXX_TRACE(logger_, "DummyDpdkPlugin destructor.");
  }

  /**
   * Configure the plugin.  This receives an IpcMessage which should be processed
   * to configure the plugin, and any response can be added to the reply IpcMessage.
   *
   * \param[in] config - Reference to the configuration IpcMessage object.
   * \param[out] reply - Reference to the reply IpcMessage object.
   */
  void DummyDpdkPlugin::configure(OdinData::IpcMessage& config, OdinData::IpcMessage& reply)
  {
    LOG4CXX_INFO(logger_, "Configuring DummyDpdk plugin");

    LOG4CXX_INFO(logger_, "Plugin name: " << this->get_name());

    // Make a copy of the config to return when the config if requested
    config_.update(config);

    FrameCallback frame_callback = boost::bind(&DummyDpdkPlugin::process_frame, this, _1);

    DpdkFrameProcessorPlugin::configure(config, reply, &decoder_, frame_callback);

  }

  void DummyDpdkPlugin::requestConfiguration(OdinData::IpcMessage& reply)
{
  // Return the configuration of the plugin
  LOG4CXX_INFO(logger_, "Configuration requested for DummyDpdk plugin");

  // Encode config_ parameters to a JSON string
  const char* config_params_json = config_.encode_params();

  // Parse the JSON string into a rapidjson::Document
  rapidjson::Document config_params_doc;
  config_params_doc.Parse(config_params_json);

  // Check for parsing errors
  if (config_params_doc.HasParseError())
  {
    throw OdinData::IpcMessageException("Failed to parse config_ parameters JSON");
  }

  reply.update(config_params_doc, "DummyDpdk");
}

  /**
   * Collate status information for the plugin.  The status is added to the status IpcMessage object.
   *
   * \param[out] status - Reference to an IpcMessage value to store the status.
   */
  void DummyDpdkPlugin::status(OdinData::IpcMessage& status)
  {
    // Record the plugin's status items
    LOG4CXX_INFO(logger_, "Status requested for DummyDpdk plugin");

    DpdkFrameProcessorPlugin::status(status);
  }

  /**
   * Reset process plugin statistics, i.e. counter of packets lost
   */
  bool DummyDpdkPlugin::reset_statistics(void)
  {
    LOG4CXX_INFO(logger_, "Statistics reset requested for DummyDpdk plugin")

    bool reset_ok = true;

    reset_ok &= DpdkFrameProcessorPlugin::reset_statistics();

    return reset_ok;
  }

  /**
   * Perform processing on the frame.  Depending on the selected bit depth
   * the corresponding pixel re-ordering algorithm is executed.
   *
   * \param[in] frame - Pointer to a Frame object.
   */
  void DummyDpdkPlugin::process_frame(boost::shared_ptr<Frame> frame)
  {
    // LOG4CXX_DEBUG(logger_, "Processing frame in DummyDpdk plugin");
    this->push(frame);
  }


} /* namespace FrameProcessor */

