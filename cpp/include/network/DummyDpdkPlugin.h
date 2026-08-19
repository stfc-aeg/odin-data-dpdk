#ifndef INCLUDE_DUMMY_DPDK_CAMERA_PLUGIN_H_
#define INCLUDE_DUMMY_DPDK_CAMERA_PLUGIN_H_

#include <string>
#include <map>

#include <log4cxx/logger.h>
using namespace log4cxx;
using namespace log4cxx::helpers;

#include <DpdkFrameProcessorPlugin.h>
#include "DummyDpdkDecoder.h"
#include "ClassLoader.h"


namespace FrameProcessor
{

  /** Reference implementation of DpdkFrameProcessorPlugin for network packet ingestion. */
  class DummyDpdkPlugin : public DpdkFrameProcessorPlugin
  {

  public:
    DummyDpdkPlugin();
    virtual ~DummyDpdkPlugin();

    void configure(OdinData::IpcMessage& config, OdinData::IpcMessage& reply);
    void requestConfiguration(OdinData::IpcMessage& reply);
    void status(OdinData::IpcMessage& status);
    bool reset_statistics(void);

    void process_frame(boost::shared_ptr<Frame> frame);

  private:

    LoggerPtr logger_;

    DummyDpdkDecoder decoder_;

    OdinData::IpcMessage config_;

    std::string current_mode_;

  };

  REGISTER(FrameProcessorPlugin, DummyDpdkPlugin, "DummyDpdkPlugin");

} /* namespace FrameProcessor */

#endif /* INCLUDE_DUMMY_DPDK_PLUGIN_H_ */
