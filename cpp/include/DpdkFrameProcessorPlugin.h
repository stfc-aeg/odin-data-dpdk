/*
 * DpdkFrameProcessorPlugin.h
 *
 *  Created on: 14 Feb 2023
 *      Author: Tim Nicholls, STFC Detector Systems Software Group
 */

#ifndef INCLUDE_DPDKFRAMEPROCESSORPLUGIN_H_
#define INCLUDE_DPDKFRAMEPROCESSORPLUGIN_H_

#include<string>
#include<map>

#include <boost/scoped_ptr.hpp>

#include <log4cxx/logger.h>
#include <log4cxx/basicconfigurator.h>
#include <log4cxx/propertyconfigurator.h>
#include <log4cxx/helpers/exception.h>
using namespace log4cxx;
using namespace log4cxx::helpers;

#include "FrameProcessorPlugin.h"
#include "ClassLoader.h"
#include "DpdkCoreManager.h"
#include "ProtocolDecoder.h"

namespace FrameProcessor
{

  /** DPDK FrameProcessor plugin
   *
   * TODO fill in
   */
  class DpdkFrameProcessorPlugin : public FrameProcessorPlugin
  {

  public:
    DpdkFrameProcessorPlugin();
    virtual ~DpdkFrameProcessorPlugin();

    int get_version_major();
    int get_version_minor();
    int get_version_patch();
    std::string get_version_short();
    std::string get_version_long();

    virtual void configure(OdinData::IpcMessage& config, OdinData::IpcMessage& reply) = 0;
    virtual void requestConfiguration(OdinData::IpcMessage& reply);
    virtual void status(OdinData::IpcMessage& status);
    virtual bool reset_statistics(void);

    virtual void process_frame(boost::shared_ptr<Frame> frame) = 0;

  protected:

    void configure(
      OdinData::IpcMessage& config, OdinData::IpcMessage& reply,
      ProtocolDecoder* decoder_ptr, FrameCallback& frame_callback
    );

  private:

    /** Pointer to logger **/
    LoggerPtr logger_;

    boost::scoped_ptr<DpdkCoreManager> core_manager_;

  };

} /* namespace FrameProcessor */

#endif /* INCLUDE_DPDKFRAMEPROCESSORPLUGIN_H_ */
