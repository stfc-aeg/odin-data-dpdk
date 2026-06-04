#include "network/DummyDpdkPlugin.h"
#include "version.h"

namespace FrameProcessor
{

  /**
   * The constructor sets up logging used within the class.
   */
  DummyDpdkPlugin::DummyDpdkPlugin() :
    DpdkFrameProcessorPlugin()
  {
    logger_ = Logger::getLogger("FP.DummyDpdkPlugin");
    LOG4CXX_INFO(logger_, "DummyDpdkPlugin version " << this->get_version_long() << " loaded");
  }

  DummyDpdkPlugin::~DummyDpdkPlugin()
  {
    LOG4CXX_TRACE(logger_, "DummyDpdkPlugin destructor.");
  }

  void DummyDpdkPlugin::configure(OdinData::IpcMessage& config, OdinData::IpcMessage& reply)
  {
    LOG4CXX_INFO(logger_, "Configuring DummyDpdk plugin: " << this->get_name());

    config_.update(config);

    FrameCallback frame_callback = boost::bind(&DummyDpdkPlugin::process_frame, this, boost::placeholders::_1);

    DpdkFrameProcessorPlugin::configure(config, reply, &decoder_, frame_callback);
  }

  void DummyDpdkPlugin::requestConfiguration(OdinData::IpcMessage& reply)
  {
    LOG4CXX_INFO(logger_, "Configuration requested for DummyDpdk plugin");

    const char* config_params_json = config_.encode_params();

    rapidjson::Document config_params_doc;
    config_params_doc.Parse(config_params_json);

    if (config_params_doc.HasParseError())
    {
      throw OdinData::IpcMessageException("Failed to parse config_ parameters JSON");
    }

    reply.update(config_params_doc, "DummyDpdk");
  }

  void DummyDpdkPlugin::status(OdinData::IpcMessage& status)
  {
    LOG4CXX_INFO(logger_, "Status requested for DummyDpdk plugin");
    DpdkFrameProcessorPlugin::status(status);
  }

  bool DummyDpdkPlugin::reset_statistics(void)
  {
    LOG4CXX_INFO(logger_, "Statistics reset requested for DummyDpdk plugin");

    bool reset_ok = true;
    reset_ok &= DpdkFrameProcessorPlugin::reset_statistics();

    return reset_ok;
  }

  void DummyDpdkPlugin::process_frame(boost::shared_ptr<Frame> frame)
  {
    this->push(frame);
  }


} /* namespace FrameProcessor */

