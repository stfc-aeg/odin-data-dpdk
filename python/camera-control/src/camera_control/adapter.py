import logging

from tornado.escape import json_decode

from odin.adapters.adapter import ApiAdapter, ApiAdapterResponse, request_types, response_types
from odin.adapters.parameter_tree import ParameterTreeError
from odin.util import decode_request_body

from camera_control.controller import CameraController, CameraError

class CameraAdapter(ApiAdapter):
    """Acquisition adapter class."""

    def __init__(self, **kwargs):
        """Initialise AcquisitionAdapter object."""

        super(CameraAdapter, self).__init__(**kwargs)

        # Split on comma, remove whitespace if it exists
        endpoints = [
            item.strip() for item in self.options.get('camera_endpoint', '').split(",") if item.strip()
        ]
        names = [
            item.strip() for item in self.options.get('camera_name', '').split(",") if item.strip()
        ]

        # check if number of endpoints = number of names
        if len(endpoints) == len(names):

            status_bg_task_enable = bool(self.options.get('status_bg_task_enable', 1))
            status_bg_task_interval = float(self.options.get('status_bg_task_interval', 1))

            # Create acquisition controller
            self.camera = CameraController(endpoints, names,
                                        status_bg_task_enable, status_bg_task_interval
                                        )
        else:
            logging.debug("Error: Number of cameras does not equal number of endpoints")

    @response_types('application/json', default='application/json')
    def get(self, path, request):
        """Handle an HTTP GET request.

        This method handles an HTTP GET request, returning a JSON response.

        :param path: URI path of request
        :param request: HTTP request object
        :return: an ApiAdapterResponse object containing the appropriate response
        """
        try:
            response = self.camera.get(path)
            status_code = 200
        except ParameterTreeError as e:
            response = {'error': str(e)}
            status_code = 400

        content_type = 'application/json'

        return ApiAdapterResponse(response, content_type=content_type,
                                  status_code=status_code)

    @request_types('application/json',"application/vnd.odin-native")
    @response_types('application/json', default='application/json')
    def put(self, path, request):
        """Handle an HTTP PUT request.

        This method handles an HTTP PUT request, returning a JSON response.

        :param path: URI path of request
        :param request: HTTP request object
        :return: an ApiAdapterResponse object containing the appropriate response
        """
        try:
            data = decode_request_body(request)
            self.camera.set(path, data)
            response = self.camera.get(path)
            content_type = "applicaiton/json"
            status = 200

        except ParameterTreeError as param_error:
            response = {'response': 'TriggerAdapter PUT error: {}'.format(param_error)}
            content_type = 'application/json'
            status = 400

        return ApiAdapterResponse(response, content_type=content_type, status_code=status)

    def delete(self, path, request):
        """Handle an HTTP DELETE request.

        This method handles an HTTP DELETE request, returning a JSON response.

        :param path: URI path of request
        :param request: HTTP request object
        :return: an ApiAdapterResponse object containing the appropriate response
        """
        response = 'LiveXAdapter: DELETE on path {}'.format(path)
        status_code = 200

        logging.debug(response)

        return ApiAdapterResponse(response, status_code=status_code)

    def cleanup(self):
        """Clean up adapter state at shutdown.

        This method cleans up the adapter state when called by the server at e.g. shutdown.
        It simplied calls the cleanup function of the LiveX instance.
        """
        self.camera.cleanup()

    def initialize(self, adapters):
        """Get list of adapters and call relevant functions for them."""
        self.adapters = dict((k, v) for k, v in adapters.items() if v is not self)
