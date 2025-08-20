from live_data.controller import LiveDataController, LiveXError
from live_data.base_adapter import BaseAdapter

import logging

from odin.adapters.adapter import (
    ApiAdapter,
    ApiAdapterResponse,
    request_types,
    response_types,
    wants_metadata,
)


class LiveDataAdapter(BaseAdapter):

    controller_cls = LiveDataController
    error_cls = LiveXError

    @response_types("application/json", default="application/json")
    def get(self, path, request):
        """Handle an HTTP GET request.

        This method handles an HTTP GET request, returning a JSON response.

        :param path: URI path of request
        :param request: HTTP request object
        :return: an ApiAdapterResponse object containing the appropriate response
        """

        try:
            parts = path.strip("/").split("/")

            if "image_data" in parts: #??
                response, content_type, status = self.controller.get_image(path) # just pass camera name?

            else:
                response = self.controller.get(path, wants_metadata(request))
                content_type = 'application/json'
                status = 200

        # except (ParameterTreeError, TypeError) as error:
        #     logging.error(error)
        #     raise LiveXError(error)

        except Exception as error: ##!!
            logging.error(error)
            # raise LiveXError(error)
            # return adapter response!!!

        return ApiAdapterResponse(response, content_type=content_type, status_code=status)
