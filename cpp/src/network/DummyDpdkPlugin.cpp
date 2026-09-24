#include "network/DummyDpdkPlugin.h"
#include "version.h"

namespace FrameProcessor
{

  /**
   * The constructor sets up logging used within the class.
   */
  DummyDpdkPlugin::DummyDpdkPlugin() :
    DpdkFrameProcessorPlugin(),
    current_mode_(decoder_.get_mode_string())
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

    // Track the currently selected mode for status reporting. The decoder
    // itself resolves "mode" per-call (see DummyDpdkDecoder::resolve_mode),
    // so this is just for status()/requestConfiguration() to report back
    // what the last configure() asked for.
    if (config.has_param("mode"))
    {
      std::string mode_str = config.get_param<std::string>("mode");
      const auto& mode_map = DummyDpdkDecoder::get_mode_string_map();
      if (mode_map.find(mode_str) != mode_map.end())
      {
        auto mode_it = mode_map.find(mode_str);

        decoder_.set_mode(mode_it->second);
        current_mode_ = mode_str;
        LOG4CXX_INFO(logger_, "Decoder mode set to: " << mode_str);
      }
      else
      {
        LOG4CXX_ERROR(logger_, "Invalid mode specified: " << mode_str);
        reply.set_param("error", "Invalid mode: " + mode_str);
      }
    }

    FrameCallback frame_callback = boost::bind(&DummyDpdkPlugin::process_frame, this, boost::placeholders::_1);

    DpdkFrameProcessorPlugin::configure(config, reply, &decoder_, frame_callback);
  }

  void DummyDpdkPlugin::requestConfiguration(OdinData::IpcMessage& reply)
  {
    // LOG4CXX_INFO(logger_, "Configuration requested for DummyDpdk plugin");

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
    const std::string plugin_name = get_name();
    DpdkFrameProcessorPlugin::status(status);

    // Report all available decoder modes.
    for (const auto& mode_pair : DummyDpdkDecoder::get_mode_string_map())
    {
      status.set_param(plugin_name + "/available_modes[]", mode_pair.first);
    }

    rapidjson::Document config_doc;
    config_doc.Parse(config_.encode_params());

    if (config_doc.HasParseError() ||
        !config_doc.HasMember("worker_cores") ||
        !config_doc["worker_cores"].IsObject())
    {
      return;
    }

    const auto& mode_map = DummyDpdkDecoder::get_mode_string_map();

    std::map<std::string, std::string> stream_modes;

    // Collect the configured mode for each stream.
    for (auto& core_member : config_doc["worker_cores"].GetObject())
    {
      const rapidjson::Value& core_cfg = core_member.value;

      if (core_cfg.HasMember("stream") &&
          core_cfg.HasMember("mode") &&
          core_cfg["stream"].IsString() &&
          core_cfg["mode"].IsString())
      {
        stream_modes[core_cfg["stream"].GetString()] =
          core_cfg["mode"].GetString();
      }
    }

    // Add mode information for each configured stream.
    for (const auto& stream_mode : stream_modes)
    {
      const std::string& stream = stream_mode.first;
      const std::string& mode_str = stream_mode.second;
      const std::string prefix = plugin_name + "/streams/" + stream;

      status.set_param(prefix + "/mode", mode_str);

      auto mode_it = mode_map.find(mode_str);

      if (mode_it == mode_map.end())
      {
        continue;
      }

      DummyDpdkDecoder mode_decoder(mode_it->second);
      const DummyModeConfiguration& cfg = mode_decoder.resolve_mode();

      // Add decoder-specific status. This will be displayed under each active mode
      status.set_param(prefix + "/mode_info/packets_per_frame", static_cast<int>(cfg.packets_per_frame));
      status.set_param(prefix + "/mode_info/payload_size", static_cast<int>(cfg.payload_size));
      status.set_param(prefix + "/mode_info/frame_outer_chunk_size", static_cast<int>(cfg.frame_outer_chunk_size));
      status.set_param(prefix + "/mode_info/frame_dimensions[]", static_cast<int>(cfg.x_resolution));
      status.set_param(prefix + "/mode_info/frame_dimensions[]", static_cast<int>(cfg.y_resolution));
      status.set_param(prefix + "/mode_info/needs_reordering", cfg.needs_reordering);
    }
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

