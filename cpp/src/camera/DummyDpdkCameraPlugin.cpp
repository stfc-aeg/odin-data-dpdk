#include "camera/DummyDpdkCameraPlugin.h"
#include "version.h"

namespace FrameProcessor
{
  DummyDpdkCameraPlugin::DummyDpdkCameraPlugin() :
    DpdkFrameProcessorPlugin()
  {
    logger_ = Logger::getLogger("FP.DummyDpdkCameraPlugin");
    LOG4CXX_INFO(logger_, "DummyDpdkCameraPlugin version " << this->get_version_long() << " loaded");
  }

  DummyDpdkCameraPlugin::~DummyDpdkCameraPlugin()
  {
    LOG4CXX_TRACE(logger_, "DummyDpdkCameraPlugin destructor.");
  }

  void DummyDpdkCameraPlugin::configure(OdinData::IpcMessage& config, OdinData::IpcMessage& reply)
  {
    LOG4CXX_INFO(logger_, "Configuring DummyDpdkCamera plugin: " << this->get_name());

    config_.update(config);

    FrameCallback frame_callback = boost::bind(&DummyDpdkCameraPlugin::process_frame, this, boost::placeholders::_1);

    DpdkFrameProcessorPlugin::configure(config, reply, &decoder_, frame_callback);
  }

  void DummyDpdkCameraPlugin::requestConfiguration(OdinData::IpcMessage& reply)
  {
    LOG4CXX_INFO(logger_, "Configuration requested for DummyDpdkCamera plugin");

    const char* config_params_json = config_.encode_params();

    rapidjson::Document config_params_doc;
    config_params_doc.Parse(config_params_json);

    if (config_params_doc.HasParseError())
    {
      throw OdinData::IpcMessageException("Failed to parse config_ parameters JSON");
    }

    reply.update(config_params_doc, "DummyDpdk");
  }

  void DummyDpdkCameraPlugin::status(OdinData::IpcMessage& status)
  {
    LOG4CXX_INFO(logger_, "Status requested for DummyDpdkCamera plugin");
    DpdkFrameProcessorPlugin::status(status);
  }

  bool DummyDpdkCameraPlugin::reset_statistics(void)
  {
    LOG4CXX_INFO(logger_, "Statistics reset requested for DummyDpdkCamera plugin");

    bool reset_ok = true;
    reset_ok &= DpdkFrameProcessorPlugin::reset_statistics();

    return reset_ok;
  }

  void DummyDpdkCameraPlugin::process_frame(boost::shared_ptr<Frame> frame)
  {
    this->push(frame);
  }
} /* namespace FrameProcessor */

