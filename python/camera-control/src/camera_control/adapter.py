import logging

from tornado.escape import json_decode

from odin.adapters.adapter import ApiAdapter, ApiAdapterResponse, request_types, response_types
from odin.adapters.parameter_tree import ParameterTreeError
from odin.util import decode_request_body

from camera_control.controller import CameraController, CameraError
from camera_control.base_adapter import BaseAdapter

class CameraAdapter(BaseAdapter):
    """Acquisition adapter class."""
    
    controller_cls = CameraController
    error_cls = CameraError

