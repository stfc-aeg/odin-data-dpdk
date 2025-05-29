from odin_data.control.ipc_channel import IpcChannel
from odin_data.control.ipc_message import IpcMessage

from odin_data.control.ipc_tornado_channel import IpcTornadoChannel

from tornado.ioloop import PeriodicCallback

from odin.adapters.parameter_tree import ParameterTree

from functools import partial
import logging

class Camera():

    def __init__(self, endpoint, name):
        """Initialises the object."""
        self.endpoint = endpoint
        self.name = name
        self.msg_id = 0

        self.status = {}
        self.config = {}

        self.connected = False
        self.config_initialised = False
        self.status_initialised = False

        self.pending_commands = {}

        # self.timeout_ms = 1000

        self.build_param_tree()

        self.connect()

    def connect(self):
        """Create an IpcTornadoChannel object and registers callbacks."""
        try:
            self.connection = IpcTornadoChannel(IpcChannel.CHANNEL_TYPE_DEALER)

            self.connection.register_monitor(self.monitor_callback)
            self.connection.register_callback(self.callback)

            self.connection.connect(self.endpoint)
        except Exception as e:
            logging.error(f"Connection error: {e}")

    def build_param_tree(self):
        """Builds the parameter tree."""
        self.tree = {}

        # Tree branches
        self.tree['camera_name'] = self.name
        self.tree['endpoint'] = self.endpoint
        self.tree['command'] = (lambda: None, self.send_command)
        self.tree['connection'] = {
            'connected': (lambda: self.connected, None)
        }
        self.tree['config'] = {}
        self.tree['status'] = {}

        self.param_tree = ParameterTree(self.tree, mutable=True)

    def _build_tree_config(self):
        """Builds config branch of parameter tree."""
        self.tree['config'] = {
            param: (
                partial(self.get_config, param=param),
                partial(self.set_config, param=param)
            )
            for param in self.config
        }
        self.param_tree.replace('config', self.tree['config'])

    def _build_tree_status(self):
        """Builds status branch of parameter tree."""
        self.tree['status'] = {
            param: (
                lambda param=param: self.status[param], None
            )
            for param in self.status
        }
        self.param_tree.replace('status', self.tree['status'])

    def monitor_callback(self, msg):
        """Handles CONNECTED and DISCONNECTED messages."""
        if msg['event'] == IpcTornadoChannel.CONNECTED:
            logging.debug("Connected")
            self.connected = True
            self.get_configuration()
            self.get_status()
        if msg['event'] == IpcTornadoChannel.DISCONNECTED:
            logging.debug("Disconnected")
            self.connected = False
            self.config_initialised = False
            self.status_initialised = False

    def callback(self, msg):
        """Handles response messages."""
        try:
            response = IpcMessage(from_str=msg[0])

            msg_val = response.get_msg_val()
            msg_type = response.get_msg_type()
            params = response.get_params()

            if msg_type == 'ack':

                if msg_val == 'request_configuration':
                    self.config = params['camera']
                    if not self.config_initialised:
                        self._build_tree_config()
                        self.config_initialised = True

                if msg_val == 'status':
                    self.status = params['camera']
                    if not self.status_initialised:
                        self._build_tree_status()
                        self.status_initialised = True

                if msg_val == 'configure':
                    sent_msg = self.pending_commands.get(response.get_msg_id())
                    sent_command = sent_msg.get_params()['command']
                    logging.debug(f"Command acknowledged: [{response.get_msg_id()}: {sent_command}]")

            else:
                logging.error(f"Error: {params['error']}")

        except Exception as e:
            logging.error(f"Callback error: {e}")

    def send_command(self, value):
        """Compose a command message to be sent to the camera.
        :param value: command to be put into the message, PUT from tree
        """

        self.send(msg_type='cmd', msg_val='configure', param='command', value=value)

        logging.debug(f"Command sent: {value}")

        state = self.param_tree.get('status/state')['state']

        if value != "stop" and state == "capturing":
            logging.error(f"Command {value} not valid whilst capturing")

        if value == "connect" and state == "connected":
            logging.error("Camera already connected")

        if value == "disconnect" and state == "disconnected":
            logging.error("Camera already disconnected")

        if value == "start" and state == "disconnected":
            logging.error("Camera must be connected before starting capture")

        if value == "stop" and state != "capturing":
            logging.error("Camera not yet capturing")

        # self.send(msg_type='cmd', msg_val='configure', param='command', value=value)

    def get_config(self, param):
        if param in self.config:
            return self.config[param]
        else:
            return None

    def set_config(self, value, param):
        """Update local storage of config values and send a command to the camera to update it.
        :param value: argument passed by PUT request to param tree
        :param param: config parameter to update, e.g.: exposure_time
        """

        self.config[param] = value

        command_msg = {
            param : value
        }

        self.send(msg_type='cmd', msg_val='configure', param='camera', value=command_msg)

    def send(self, msg_type, msg_val, param=None, value=None):
        """Construct and send an IPC message.
        :param msg_type: message type
        :param msg_val: message value
        :param param: parameter to be set
        :param value: argument to set param to
        """

        msg = IpcMessage(msg_type, msg_val, id=self._next_msg_id())
        msg.set_param(param, value)
        self.connection.send(msg.encode())

        self.pending_commands[msg.get_msg_id()] = msg

    def get_configuration(self):
        """Get the configuration of the camera."""

        self.send(msg_type='cmd', msg_val='request_configuration')

    def get_status(self):
        """Get the status of the camera."""

        self.send(msg_type='cmd', msg_val='status')

    def _next_msg_id(self):
        """Return the next (incremented) message id."""
        self.msg_id += 1
        return self.msg_id
