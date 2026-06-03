## Overview

The CameraControlCore is a DPDK-based worker core that provides an IPC control interface for the camera subsystem. It runs a ZeroMQ ROUTER channel bound to a fixed TCP port and dispatches incoming odin-data `IpcMessage` requests to the `CameraController` singleton. This separates camera command handling (configure, status, request configuration) from the high-throughput capture path managed by `CameraCaptureCore`.

At construction, the CameraControlCore creates the `CameraController` singleton and passes it the decoder and the `camera_config` block from the startup configuration.

## Configuration

The CameraControlCore accepts the following configuration parameters through the odin-data-dpdk startup config:

```json
"camera_control": {
    "core_name": "CameraControlCore",
    "num_cores": 1,
    "connect": "",
    "upstream_core": "",
    "num_downstream_cores": 0,
    "frame_timeout": 1000,
    "exposure_time": 0.01,
    "frame_rate": 100.0,
    "camera_config": { }
}
```

### Configuration Parameters

| Parameter              | Type    | Description                                                                               |
| ---------------------- | ------- | ----------------------------------------------------------------------------------------- |
| `core_name`            | string  | Name of the class for this core type                                                      |
| `num_cores`            | integer | Number of CameraControlCore instances to create                                           |
| `connect`              | string  | Upstream core name (unused; this core has no upstream ring)                               |
| `upstream_core`        | string  | Upstream core name (unused; this core has no upstream ring)                               |
| `num_downstream_cores` | integer | Number of downstream cores (typically 0)                                                  |
| `frame_timeout`        | integer | Frame timeout in milliseconds                                                             |
| `exposure_time`        | double  | Exposure time in seconds, forwarded to the camera on construction                        |
| `frame_rate`           | double  | Frame rate in Hz, forwarded to the camera on construction                                 |
| `camera_config`        | object  | Camera-specific configuration block passed directly to the `CameraController`            |

## Connections

### Upstream Connections

**None** — the CameraControlCore does not connect to any DPDK ring. Control messages arrive via the ZeroMQ IPC channel.

### Downstream Connections

**None** — the CameraControlCore does not enqueue frames to any downstream ring.

## IPC Control Channel

The core binds a ZeroMQ ROUTER socket to `tcp://127.0.0.1:9001` at startup and polls it with a 10 ms timeout each iteration. Incoming `IpcMessage` requests are decoded and dispatched:

| Message Value              | Handler                                           |
| -------------------------- | ------------------------------------------------- |
| `MsgValCmdConfigure`       | `CameraController_->configure(ctrl_req, ctrl_reply)` |
| `MsgValCmdRequestConfiguration` | `CameraController_->request_configuration("", ctrl_reply)` |
| `MsgValCmdStatus`          | `CameraController_->get_status("", ctrl_reply)`   |

Unsupported message types or values result in a NACK response with an error description. Replies are routed back to the originating client using the ZeroMQ client identity.

## Processing Loop

The core runs a control polling loop in the `run()` method:

1. **Poll**: Calls `Camera_Ctrl_Channel_.poll(10)` with a 10 ms timeout.
2. **Receive**: If a message is available, receives the encoded IPC message and the client identity for routing the reply.
3. **Decode**: Parses the message type and value from the `IpcMessage`.
4. **Dispatch**: Routes the request to the appropriate `CameraController` method.
5. **Reply**: Sends the encoded reply back to the client via the ROUTER socket.
6. **Error Handling**: Catches `IpcMessageException` and sends a NACK response if decoding fails.

## Statistics and Monitoring

The CameraControlCore reports status under the path `<base_path>/CameraControlCore_<idx>/`. Detailed camera state is reported by the `CameraController` directly and is accessible via the `MsgValCmdStatus` and `MsgValCmdRequestConfiguration` IPC commands.
