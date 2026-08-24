#include "camera/CameraStateMachine.h"
#include "camera/DpdkCamera.h"

namespace sc = boost::statechart;

namespace FrameProcessor {

    CameraStateMachine::CameraStateMachine(DpdkCamera* camera) :
    camera_(camera)
{
    // State transition events
    EventConnect::custom_static_type_ptr("connect");
    EventDisconnect::custom_static_type_ptr("disconnect");
    EventStartCapture::custom_static_type_ptr("capture");
    EventEndCapture::custom_static_type_ptr("end_capture");
    
    // Initialize the state machine
    this->initiate();
}

void CameraStateMachine::execute_command(const char* command)
{
    std::string cmd_str(command);
    this->execute_command(cmd_str);
}

void CameraStateMachine::execute_command(std::string& command)
{
    CommandType command_type = map_command_to_type(command);

    if (command_type == CommandUnknown)
    {
        std::stringstream ss;
        ss << "Unknown camera state transition command: " << command;
        throw(std::runtime_error(ss.str()));
    }
    this->execute_command(command_type);
}

void CameraStateMachine::execute_command(CameraStateMachine::CommandType command)
{
    // Use std::lock_guard instead of boost::lock_guard
    std::lock_guard<std::mutex> transition_lock(state_transition_mutex_);

    switch (command)
    {
        case CommandConnect:
            process_event(EventConnect());
            break;
        case CommandDisconnect:
            process_event(EventDisconnect());
            break;
        case CommandStartCapture:
            process_event(EventStartCapture());
            break;
        case CommandEndCapture:
            process_event(EventEndCapture());
            break;
        case CommandUnknown:
        default:
            {
                std::stringstream ss;
                ss << "Unknown camera state transition command type: " << (int)command;
                throw(std::runtime_error(ss.str()));
            }
            break;
    }
}

void CameraStateMachine::unconsumed_event(const sc::event_base& event)
{
    // Called synchronously by boost::statechart from inside process_event(), which
    // execute_command() invokes while already holding state_transition_mutex_. Using
    // current_state_name() here would try to re-lock that same non-recursive mutex and deadlock,
    // so the state is read via the unlocked helper instead.
    std::stringstream ss;
    ss << event.custom_dynamic_type_ptr<char>() << " is not valid in "
        << map_state_to_name(current_state_locked()) << " state";

    throw(std::runtime_error(ss.str()));
}

CameraStateMachine::CommandType CameraStateMachine::map_command_to_type(std::string& command)
{
    CommandType command_type = CommandUnknown;

    if (command_type_map_.size() == 0)
    {
        init_command_type_map();
    }

    if (command_type_map_.left.count(command))
    {
        command_type = command_type_map_.left.at(command);
    }
    return command_type;
}

std::string CameraStateMachine::map_state_to_name(StateType state_type)
{
    std::string state_name = "unknown";

    if (state_type_map_.size() == 0)
    {
        init_state_type_map();
    }

    if (state_type_map_.right.count(state_type))
    {
        state_name = state_type_map_.right.at(state_type);
    }
    return state_name;
}

std::string CameraStateMachine::current_state_name(void)
{
    return map_state_to_name(current_state());
}

CameraStateMachine::StateType CameraStateMachine::current_state(void)
{
    // state_cast() walks the boost::statechart internal state tree, which execute_command()
    // mutates via process_event() while holding this same mutex. Without this lock, a reader on
    // another thread (e.g. CameraCaptureCore's run loop, polling get_state_name() every iteration)
    // can observe the machine mid-transition and state_cast<IStateInfo&>() throws std::bad_cast,
    // which is unhandled here and aborts the process.
    std::lock_guard<std::mutex> transition_lock(state_transition_mutex_);
    return current_state_locked();
}

CameraStateMachine::StateType CameraStateMachine::current_state_locked(void)
{
    return state_cast<const IStateInfo&>().state_type();
}

void CameraStateMachine::init_command_type_map(void)
{
    command_type_map_.insert(CommandTypeMapEntry("connect",      CommandConnect));
    command_type_map_.insert(CommandTypeMapEntry("disconnect",   CommandDisconnect));
    command_type_map_.insert(CommandTypeMapEntry("capture", CommandStartCapture));
    command_type_map_.insert(CommandTypeMapEntry("end_capture",  CommandEndCapture));
}

void CameraStateMachine::init_state_type_map(void)
{
    state_type_map_.insert(StateTypeMapEntry("disconnected", StateOff));
    state_type_map_.insert(StateTypeMapEntry("connected",    StateConnected));
    state_type_map_.insert(StateTypeMapEntry("capturing",    StateCapturing));
}

sc::result Off::react(const EventConnect&)
{
    if (outermost_context().camera_->connect())
    {
        return transit<Connected>();
    }
    else
    {
        // Return discard_event() if the connect operation fails
        return discard_event();
    }
}

sc::result Connected::react(const EventDisconnect&)
{
    if (outermost_context().camera_->disconnect())
    {
        return transit<Off>();
    }
    else
    {
        // Return discard_event() if the disconnect operation fails
        return discard_event();
    }
}

sc::result Connected::react(const EventStartCapture&)
{
    if (outermost_context().camera_->start_capture())
    {
        return transit<Capturing>();
    }
    else
    {
        // Return discard_event() if the start capture operation fails
        return discard_event();
    }
}

sc::result Capturing::react(const EventEndCapture&)
{
    if (outermost_context().camera_->end_capture())
    {
        return transit<Connected>();
    }
    else
    {
        // Return discard_event() if the end capture operation fails
        return discard_event();
    }
}

} // namespace FrameProcessor