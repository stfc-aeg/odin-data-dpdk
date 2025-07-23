/*
 * DummyDpdkCameraPlugin.h
 *
 *  Created on: 18 September 2024
 *      Author: Dominic Banks, STFC Detector Systems Software Group
 */

#ifndef INCLUDE_DUMMY_DPDK_CAMERA_PLUGIN_H_
#define INCLUDE_DUMMY_DPDK_CAMERA_PLUGIN_H_

#include<string>
#include<map>

#include <boost/scoped_ptr.hpp>

#include <log4cxx/logger.h>
#include <log4cxx/basicconfigurator.h>
#include <log4cxx/propertyconfigurator.h>
#include <log4cxx/helpers/exception.h>
using namespace log4cxx;
using namespace log4cxx::helpers;

#include <DpdkFrameProcessorPlugin.h>
#include "camera/DummyDpdkCameraDecoder.h"
#include "ClassLoader.h"


namespace FrameProcessor
{

  /** DummyDpdk Plugin
   *
   * The DummyDpdkCameraPlugin class implements a DPDK-aware plugin capable of receiving data
   * frame packets from upstream DPDK packet processing cores and injecting them into the
   * frameProcessor frame data flow.
   */
  class DummyDpdkCameraPlugin : public DpdkFrameProcessorPlugin
  {

  public:
    DummyDpdkCameraPlugin();
    virtual ~DummyDpdkCameraPlugin();

    void configure(OdinData::IpcMessage& config, OdinData::IpcMessage& reply);
    void requestConfiguration(OdinData::IpcMessage& reply);
    void status(OdinData::IpcMessage& status);
    bool reset_statistics(void);

    void process_frame(boost::shared_ptr<Frame> frame);

  private:

    /** Pointer to logger **/
    LoggerPtr logger_;

    DummyDpdkCameraProtocolDecoder decoder_;

    OdinData::IpcMessage config_;

  };

  /**
   * Registration of this plugin through the ClassLoader.  This macro
   * registers the class without needing to worry about name mangling
   */
  REGISTER(FrameProcessorPlugin, DummyDpdkCameraPlugin, "DummyDpdkCameraPlugin");

} /* namespace FrameProcessor */

#endif /* INCLUDE_DUMMY_DPDK_CAMERA_PLUGIN_H_ */
