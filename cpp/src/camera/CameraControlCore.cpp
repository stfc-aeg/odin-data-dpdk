#include "camera/CameraControlCore.h"
#include <iostream>
#include <unordered_map>

#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_udp.h>
#include <rte_malloc.h>
#include "DpdkUtils.h"
#include "ParamContainer.h"


namespace FrameProcessor
{
    CameraControlCore::CameraControlCore(
        int proc_idx, int socket_id, DpdkWorkCoreReferences dpdkWorkCoreReferences
    ) :
        DpdkWorkerCore(socket_id),
        proc_idx_(proc_idx),
        decoder_(dpdkWorkCoreReferences.decoder),
        shared_buf_(dpdkWorkCoreReferences.shared_buf),
        logger_(Logger::getLogger("FP.CameraControlCore")),
        Camera_Ctrl_Channel_(ZMQ_ROUTER), // don't know which type of IPC channel to create, this may need changing.
        CameraController_(NULL)
    {
        LOG4CXX_INFO(logger_, "Core CameraControlCore" << proc_idx_ << " creation!");

        // Resolve configuration parameters for this core from the config object passed as an
        // argument, and the current port ID
        config_.resolve(dpdkWorkCoreReferences.core_config);

        LOG4CXX_INFO(logger_, "Core CameraControlCore" << proc_idx_ << " config resolved!");

        // Resolve the control core and camera configuration from the config object
        // TODO the core name is hardcoded here and must match config file - can manager pass in as argument?
        const rapidjson::Value* control_config = dpdkWorkCoreReferences.core_config.get_worker_core_config("camera_control");
        const rapidjson::Value& camera_config = (*control_config)["camera_config"];


        CameraController_ = CameraController::Instance("CameraController_", decoder_, camera_config);

        

    }

    CameraControlCore::~CameraControlCore()
    {
        LOG4CXX_DEBUG_LEVEL(2, logger_, "CameraControlCore destructor");

        // Stop the core polling loop so the run method terminates
        stop();
    }

    bool CameraControlCore::run(unsigned int lcore_id)
    {

        lcore_id_ = lcore_id;
        run_lcore_ = true;

        LOG4CXX_INFO(logger_, "Core " << lcore_id_ << " starting up");


        // Bind the IPC channel
        // TODO: Move the address to a config param
        Camera_Ctrl_Channel_.bind("tcp://0.0.0.0:9001");


        LOG4CXX_INFO(logger_, "Core " << lcore_id_ << " Bound IPC channel to port 9001");

        bool new_msg = false;

        // Setup before fast loop

        std::string latest_message;
        std::string mesg_id;

        while (likely(run_lcore_))
        {
            // Do fast loop
            // TODO: Make this poll time a config param
            new_msg = Camera_Ctrl_Channel_.poll(10);

            if(new_msg)
            {
                // Receive the control channel request and store the client identity so that the response
                // can be routed back correctly.
                std::string client_identity;
                std::string ctrl_req_encoded = Camera_Ctrl_Channel_.recv(&client_identity);

                // Create a reply message
                OdinData::IpcMessage ctrl_reply;
                OdinData::IpcMessage::MsgVal ctrl_reply_val = OdinData::IpcMessage::MsgValIllegal;

                bool request_ok = true;
                std::ostringstream error_ss;

                std::stringstream ss;

                try
                {

                    // Attempt to decode the incoming message and get the request type and value
                    OdinData::IpcMessage ctrl_req(ctrl_req_encoded.c_str(), false);
                    OdinData::IpcMessage::MsgType req_type = ctrl_req.get_msg_type();
                    OdinData::IpcMessage::MsgVal req_val = ctrl_req.get_msg_val();

                    // Pre-populate the appropriate fields in the response
                    ctrl_reply.set_msg_id(ctrl_req.get_msg_id());
                    ctrl_reply.set_msg_type(OdinData::IpcMessage::MsgTypeAck);
                    ctrl_reply.set_msg_val(req_val);

                    // Handle the request according to its type
                    switch (req_type)
                    {
                    // Handle command reqests
                    case OdinData::IpcMessage::MsgTypeCmd:

                        // Handle command requests according to their value
                        switch (req_val)
                        {
                        // Handle a configuration command
                        case OdinData::IpcMessage::MsgValCmdConfigure:
                            
                            ss << ": Got camera control configure request from client " << client_identity
                                << " : " << ctrl_req_encoded << std::endl;
                            LOG4CXX_INFO(logger_, "Core " << lcore_id_ << ss.str());


                            CameraController_->configure(ctrl_req, ctrl_reply);
                            break;

                        // Handle a configuration request command
                        case OdinData::IpcMessage::MsgValCmdRequestConfiguration:
                            
                            ss << " Got camera control read configuration request from client " << client_identity
                            << " : " << ctrl_req_encoded << std::endl;

                            LOG4CXX_INFO(logger_, "Core " << lcore_id_ << ss.str());

                            CameraController_->request_configuration(std::string(""), ctrl_reply);
                            break;

                        // Handle a status request command
                        case OdinData::IpcMessage::MsgValCmdStatus:

                            ss << " Got camera control status request from client " << client_identity
                                << " : " << ctrl_req_encoded << std::endl;

                            LOG4CXX_INFO(logger_, "Core " << lcore_id_ << ss.str());

                            CameraController_->get_status(std::string(""), ctrl_reply);
                            break;

                        // Handle unsupported request values by setting the status and error message
                        default:
                            request_ok = false;
                            error_ss << "Illegal command request value: " << req_val;
                            break;
                        }
                        break;

                    // Handle unsupported request types by setting the status and error message
                    default:
                        request_ok = false;
                        error_ss << "Illegal command request type: " << req_type;
                        break;
                    }
                }

                // Handle exceptions thrown during message decoding, setting the status and error message
                // accordingly
                catch (OdinData::IpcMessageException& e)
                {
                    request_ok = false;
                    error_ss << e.what();
                }

                // If the request could not be decoded or handled, set the response type to NACK and populate
                // the error parameter with the error string
                if (!request_ok) {
                    LOG4CXX_ERROR(logger_, "Error handling camera control channel request from client "
                                << client_identity << ": " << error_ss.str());
                    ctrl_reply.set_nack(error_ss.str());
                }

                // Send the encoded response back to the client
                Camera_Ctrl_Channel_.send(ctrl_reply.encode(), 0, client_identity);
            }
        }
        return true;
    }

    void CameraControlCore::stop(void)
    {
        if (run_lcore_)
        {
            LOG4CXX_INFO(logger_, "Core " << lcore_id_ << " stopping");

            run_lcore_ = false;
        }
        else
        {
            LOG4CXX_DEBUG_LEVEL(2, logger_, "Core " << lcore_id_ << " already stopped");
        }
    }

    void CameraControlCore::status(OdinData::IpcMessage& status, const std::string& path)
    {
        LOG4CXX_DEBUG(logger_, "Status requested for Camera_Capture_Core_" << proc_idx_
            << " from the DPDK plugin");

        std::string status_path = path + "/CameraControlCore_" + std::to_string(proc_idx_) + "/";

        // Add any values being monitored here
    }

    bool CameraControlCore::connect(void)
    {
        LOG4CXX_INFO(logger_, "Core " << proc_idx_ << " connecting...");

        // This should should up any required resources before the main fast loop starts
        // For example you could look up the upstream rings that may be required


        return true;
    }


    void CameraControlCore::configure(OdinData::IpcMessage& config)
    {
        // Update the config based from the passed OdinData::IpcMessage

        LOG4CXX_INFO(logger_, config_.core_name << " : " << lcore_id_ << " Got update config.");
    }

    DPDKREGISTER(DpdkWorkerCore, CameraControlCore, "CameraControlCore");
}