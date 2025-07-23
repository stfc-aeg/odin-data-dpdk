"""Class to control camera."""

from odin.adapters.parameter_tree import ParameterTree, ParameterTreeError
from camera_control.camera import Camera

from tornado.ioloop import PeriodicCallback

from camera_control.base_controller import BaseController, BaseError

import logging

class CameraError(BaseError):
    """Simple exception class to wrap lower-level exceptions."""
    pass

class CameraController(BaseController):
    """Class to consolidate camera controls."""

    # def __init__(self, endpoints, names, status_bg_task_enable, status_bg_task_interval):
    def __init__(self, options):
        """This constructor initialises the object, builds the parameter tree, and starts background tasks."""

        print(options)

        endpoints = [
            item.strip() for item in options.get('camera_endpoint', None).split(",")
        ]
        names = [
            item.strip() for item in options.get('camera_name', None).split(",")
        ]

        status_bg_task_enable = bool(options.get('status_bg_task_enable', None))
        status_bg_task_interval = int(options.get('status_bg_task_interval', None))

        # Internal variables
        self.cameras = {}

        self.endpoints = endpoints
        self.names = names

        self.status_bg_task_enable = status_bg_task_enable
        self.status_bg_task_interval = status_bg_task_interval

        self._connect_cameras()

        self.param_tree = ParameterTree({
            'cameras': {camera: self.cameras[camera].param_tree for camera in self.cameras},
            'background_task': {
                "interval": (lambda: self.status_bg_task_interval, self.set_task_interval),
                "enable": (lambda: self.status_bg_task_enable, self.set_task_enable)
            }
        })

        if self.status_bg_task_enable:
            self.start_background_tasks()

    def initialize(self, adapters):
        return super().initialize(adapters)

    def _connect_cameras(self):
        """Attempt to connect cameras."""

        for i in range(len(self.endpoints)):
            self.cameras[self.names[i]] = Camera(self.endpoints[i], self.names[i])

    def get(self, path, wants_metadata=False):
        """Get the parameter tree.
        This method returns the parameter tree.
        :param path: path to retrieve from tree
        """
        return self.param_tree.get(path, wants_metadata)

    def set(self, path, data):
        """Set parameters in the parameter tree.
        This method simply wraps underlying ParameterTree method.
        :param path: path of parameter tree to set values for
        :param data: dictionary of new data values to set in the parameter tree
        """
        try:
            self.param_tree.set(path, data)
        except ParameterTreeError as e:
            raise CameraError(e)

    def status_ioloop_callback(self):
        """Periodic callback task to update each cameras configuration and status."""
        for camera in self.cameras:
            camera = self.cameras[camera]
            if camera.connected:
                camera.get_configuration()
                camera.get_status()

    def start_background_tasks(self):
        """Start the background task."""
        self.status_ioloop_task = PeriodicCallback(
            self.status_ioloop_callback, (self.status_bg_task_interval * 1000)
        )
        self.status_ioloop_task.start()

    def stop_background_tasks(self):
        """Stop the background task."""
        self.status_bg_task_enable = False
        self.status_ioloop_task.stop()

    def set_task_enable(self, enable):
        """Set the background task enable - accordingly enable or disable the task."""
        enable = bool(enable)

        if enable != self.status_bg_task_enable:
            if enable:
                self.start_background_tasks()
            else:
                self.stop_background_tasks()

    def set_task_interval(self, interval):
        """Set the background task interval."""
        logging.debug("Setting background task interval to %f", interval)
        self.status_bg_task_interval = float(interval)

    def cleanup(self):
        return super().cleanup()
