#include "camera/CameraControlCore.h"
#include "DpdkUtils.h"
#include <stdexcept>


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
        requests_received_(0),
        requests_failed_(0),
        configure_requests_(0),
        status_requests_(0),
        config_requests_(0),
        channel_bound_(false),
        CameraController_(NULL)
    {
        config_.resolve(dpdkWorkCoreReferences.core_config);

        LOG4CXX_INFO(logger_, "CameraControlCore " << proc_idx_ << " Created");

        // "camera_control" key is hardcoded — must match the JSON config file key
        const rapidjson::Value* control_config = dpdkWorkCoreReferences.core_config.get_worker_core_config("camera_control");
        if (control_config == NULL)
        {
            throw std::runtime_error(
                "CameraControlCore: no 'camera_control' section found in worker core config"
            );
        }
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


        Camera_Ctrl_Channel_.bind(config_.zmq_address_);
        channel_bound_ = true;
        LOG4CXX_INFO(logger_, "Core " << lcore_id_ << " Bound IPC channel to " << config_.zmq_address_);

        bool new_msg = false;

        while (likely(run_lcore_))
        {
            new_msg = Camera_Ctrl_Channel_.poll(10);

            if (new_msg)
            {
                // Receive the control channel request and store the client identity so that the response
                // can be routed back correctly.
                std::string client_identity;
                std::string ctrl_req_encoded = Camera_Ctrl_Channel_.recv(&client_identity);

                requests_received_++;
                last_client_ = client_identity;

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

                    switch (req_type)
                    {
                    case OdinData::IpcMessage::MsgTypeCmd:
                        switch (req_val)
                        {
                        case OdinData::IpcMessage::MsgValCmdConfigure:
                            LOG4CXX_DEBUG(logger_, "Core " << lcore_id_ << ": Configure from " << client_identity);
                            configure_requests_++;
                            CameraController_->configure(ctrl_req, ctrl_reply);
                            break;

                        case OdinData::IpcMessage::MsgValCmdRequestConfiguration:
                            LOG4CXX_DEBUG(logger_, "Core " << lcore_id_ << ": RequestConfiguration from " << client_identity);
                            config_requests_++;
                            CameraController_->request_configuration(std::string(""), ctrl_reply);
                            break;

                        case OdinData::IpcMessage::MsgValCmdStatus:
                            LOG4CXX_DEBUG(logger_, "Core " << lcore_id_ << ": Status from " << client_identity);
                            status_requests_++;
                            CameraController_->get_status(std::string(""), ctrl_reply);
                            break;

                        default:
                            request_ok = false;
                            error_ss << "Illegal command request value: " << req_val;
                            break;
                        }
                        break;

                    default:
                        request_ok = false;
                        error_ss << "Illegal command request type: " << req_type;
                        break;
                    }
                }
                catch (OdinData::IpcMessageException& e)
                {
                    request_ok = false;
                    error_ss << e.what();
                }

                if (!request_ok)
                {
                    LOG4CXX_ERROR(logger_, "Error handling camera control request from " << client_identity << ": " << error_ss.str());
                    ctrl_reply.set_nack(error_ss.str());
                    requests_failed_++;
                    last_error_ = error_ss.str();
                }

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
        // CameraControlCoreConfiguration has no config_key/stream_id binding, and the control core
        // is a single instance driving the shared CameraController, so the class name is used here
        // rather than the per-stream scoping applied to the capture core.
        std::string status_path = path + "/CameraControlCore_" + std::to_string(proc_idx_) + "/";
        std::string control_status = status_path + "control_channel/";
        std::string camera_status = status_path + "camera/";

        status.set_param(status_path + "lcore_id", (int)lcore_id_);
        status.set_param(status_path + "running", run_lcore_);

        status.set_param(control_status + "zmq_address", config_.zmq_address_);
        status.set_param(control_status + "bound", channel_bound_);
        status.set_param(control_status + "requests_received", requests_received_);
        status.set_param(control_status + "requests_failed", requests_failed_);
        status.set_param(control_status + "configure_requests", configure_requests_);
        status.set_param(control_status + "status_requests", status_requests_);
        status.set_param(control_status + "request_configuration_requests", config_requests_);
        status.set_param(control_status + "last_client", last_client_);
        status.set_param(control_status + "last_error", last_error_);

        // Camera state as owned by the controller this core drives. The camera's own status
        // container is collected into a scratch message first: get_status() nacks the message it is
        // given if the camera is missing, and nacking the shared plugin status reply here would
        // discard every other core's status alongside it.
        if (CameraController_ != NULL)
        {
            status.set_param(camera_status + "recording", CameraController_->get_recording());

            OdinData::IpcMessage camera_reply;
            if (CameraController_->get_status(camera_status, camera_reply))
            {
                // Merge the typed params across rather than embedding an encoded blob. get_status()
                // already applied camera_status as the prefix, so the nesting is preserved.
                status.update(camera_reply);
            }
        }
    }

    bool CameraControlCore::connect(void)
    {
        LOG4CXX_INFO(logger_, "Core " << proc_idx_ << " connecting...");
        return true;
    }

    void CameraControlCore::configure(OdinData::IpcMessage& config)
    {
        LOG4CXX_INFO(logger_, config_.core_name << " : " << lcore_id_ << " Got update config.");
    }

    void CameraControlCore::requestConfiguration(OdinData::IpcMessage& reply)
    {
        LOG4CXX_DEBUG(logger_, "Configuration requested for worker core");
    }

    DPDKREGISTER(DpdkWorkerCore, CameraControlCore, "CameraControlCore");
}
