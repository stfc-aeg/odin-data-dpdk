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
                            CameraController_->configure(ctrl_req, ctrl_reply);
                            break;

                        case OdinData::IpcMessage::MsgValCmdRequestConfiguration:
                            LOG4CXX_DEBUG(logger_, "Core " << lcore_id_ << ": RequestConfiguration from " << client_identity);
                            CameraController_->request_configuration(std::string(""), ctrl_reply);
                            break;

                        case OdinData::IpcMessage::MsgValCmdStatus:
                            LOG4CXX_DEBUG(logger_, "Core " << lcore_id_ << ": Status from " << client_identity);
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
        std::string status_path = path + "/CameraControlCore_" + std::to_string(proc_idx_) + "/";
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

    DPDKREGISTER(DpdkWorkerCore, CameraControlCore, "CameraControlCore");
}
