## Overview

The CameraCaptureCore is a DPDK-based worker core for camera-based data acquisition. Rather than receiving data from the network, it integrates with a `CameraController` to pull frames directly from a camera plugin (e.g., a hardware camera or the `SimulatedDpdkCamera`). When the camera status indicates an active capture, the core retrieves each frame pointer from the controller, copies the pixel data into a hugepages frame buffer from the shared memory pool, and enqueues the buffer to downstream processing cores.

The CameraCaptureCore also exposes `pop_empty_buffer()` and `push_empty_buffer()` methods so that the `CameraController` can perform zero-copy transfers directly into hugepages memory when the camera plugin supports it.

## Configuration

The CameraCaptureCore accepts the following configuration parameters through the odin-data-dpdk startup config:

```json
"camera_capture": {
    "core_name": "CameraCaptureCore",
    "num_cores": 1,
    "connect": "",
    "upstream_core": "",
    "num_downstream_cores": 1,
    "frame_timeout": 1000,
    "camera_name": "SimulatedDpdkCamera"
}
```

### Configuration Parameters

| Parameter              | Type    | Description                                                                               |
| ---------------------- | ------- | ----------------------------------------------------------------------------------------- |
| `core_name`            | string  | Name of the class for this core type                                                      |
| `num_cores`            | integer | Number of CameraCaptureCore instances to create                                           |
| `connect`              | string  | Upstream core name (unused; CameraCaptureCore has no upstream ring)                       |
| `upstream_core`        | string  | Upstream core name (unused; CameraCaptureCore has no upstream ring)                       |
| `num_downstream_cores` | integer | Number of downstream cores to distribute captured frames to                               |
| `frame_timeout`        | integer | Frame timeout in milliseconds (default: 1000)                                             |
| `camera_name`          | string  | Class name of the camera plugin to load (default: `SimulatedDpdkCamera`)                 |

## Connections

### Upstream Connections

**None** — the CameraCaptureCore acts as a data source, pulling frames from the camera via the `CameraController` rather than from an upstream DPDK ring.

- **Clear Frames Ring** (`clear_frames_ring_`): Created and pre-populated with hugepages buffer addresses from the `DpdkSharedBuffer`. Buffers are dequeued here when a frame needs to be captured, and returned via `push_empty_buffer()` when the downstream pipeline has finished with them.

### Downstream Connections

- **Downstream Rings** (`downstream_rings_`): One ring per downstream core, named `<core_name>_<socket_id>_<ring_idx>`. Captured `SuperFrameHeader*` buffers are enqueued using `frame_number % num_downstream_cores`.

## Runtime Configuration

The CameraCaptureCore delegates all runtime configuration to the `CameraController`. Configuration updates received via the odin-data IPC channel are logged but not currently processed directly by this core.

## Processing Loop

The core runs a continuous frame capture loop in the `run()` method:

1. **Controller Acquisition**: At startup, looks up the singleton `CameraController` instance and registers itself as the capture core.
2. **Capture Guard**: Each loop iteration checks that the camera status is `"capturing"` and that either unlimited frames are requested (`num_frames == 0`) or the current frame number is within the configured acquisition count.
3. **Frame Retrieval**: Calls `camera_controller_->get_frame()` to obtain a pointer to the latest frame data from the camera.
4. **Buffer Allocation**: Dequeues a hugepages frame buffer from `clear_frames_ring_`. If none is available, the frame is dropped and `dropped_frames_` is incremented.
5. **Frame Copy**: Zeros the frame header, sets the frame number and start timestamp, then uses `rte_memcpy()` to copy the camera pixel data into the hugepages buffer at the image data offset.
6. **Image Size**: Sets the image size in the super-frame header via `decoder_->set_super_frame_image_size()`.
7. **Downstream Enqueue**: Enqueues the buffer to the appropriate downstream ring.
8. **Frame Counter**: Increments `camera_status_->frame_number_` after each successful capture.

## Statistics and Monitoring

The CameraCaptureCore reports status under the path `<base_path>/CameraCaptureCore_<idx>/`. Detailed camera statistics are reported by the `CameraController` and the underlying camera plugin rather than by this core directly.
