## Overview

The PythonAccessCore is a DPDK-based worker core that exposes assembled frames to Python code via named DPDK rings. It receives `SuperFrameHeader*` buffers from an upstream core and enqueues them onto a set of `PythonRingBuffer` rings that Python processes can dequeue from directly. After Python code has processed or inspected a frame, it is expected to re-enqueue the buffer onto the appropriate downstream ring for continued pipeline processing.

This core is intended as an inspection or processing hook that allows Python-based analysis or modification of frame data at line rate without leaving the hugepages memory domain.

## Configuration

The PythonAccessCore accepts the following configuration parameters through the odin-data-dpdk startup config:

```json
"python_access": {
    "core_name": "PythonAccessCore",
    "num_cores": 1,
    "connect": "FrameBuilderCore",
    "upstream_core": "FrameBuilderCore",
    "num_downstream_cores": 1
}
```

### Configuration Parameters

| Parameter              | Type    | Description                                                                  |
| ---------------------- | ------- | ---------------------------------------------------------------------------- |
| `core_name`            | string  | Name of the class for this core type                                         |
| `num_cores`            | integer | Number of PythonAccessCore instances to create                               |
| `connect`              | string  | Name of the upstream core whose rings to connect to                          |
| `upstream_core`        | string  | Name used to look up the upstream frame ring                                 |
| `num_downstream_cores` | integer | Number of downstream cores (also determines the number of Python ring buffers created) |

## Connections

### Upstream Connections

- **Upstream Ring** (`upstream_ring_`): One ring per instance, named after the upstream core. Receives `SuperFrameHeader*` buffers for Python exposure.
- **Clear Frames Ring** (`clear_frames_ring_`): Shared hugepages buffer pool. One buffer is dequeued at startup for internal use.

### Downstream Connections

- **Python Access Rings** (`python_access_rings_`): One ring per downstream core, named `PythonRingBuffer_<socket_id>_<ring_idx>`. Frames are enqueued here for Python processes to dequeue. Python code should dequeue frames, modify or inspect them, and re-enqueue to the appropriate downstream consumer ring.
- **Downstream Rings** (`downstream_rings_`): One ring per downstream core, named `<core_name>_<socket_id>_<ring_idx>`. Available for direct forwarding if Python processing is bypassed.

## Processing Loop

The core runs a continuous frame routing loop in the `run()` method:

1. **Frame Dequeue**: Attempts to dequeue a `SuperFrameHeader*` from the upstream ring. Increments `idle_loops_` and retries if none is available.
2. **Python Enqueue**: Enqueues the frame buffer to the Python access ring selected by `frame_number % num_downstream_cores`.
3. **Python Handoff**: Python code is expected to dequeue the frame from the `PythonRingBuffer`, process it, and place it onto the correct downstream ring for continued pipeline processing.

## Statistics and Monitoring

The PythonAccessCore reports the following metrics under the path `<base_path>/PythonAccessCore_<idx>/`:

| Metric                              | Description                                                     |
| ----------------------------------- | --------------------------------------------------------------- |
| `frames_processed`                  | Total frames routed to the Python access rings                  |
| `frames_processed_per_second`       | Frames routed in the last second                                |
| `last_frame_number`                 | Frame number of the most recently routed frame                  |
| `idle_loops`                        | Main loop iterations where no frame was available               |
| `core_usage`                        | Core utilisation (0–255 scale, updated every second)            |
| `timing/mean_frame_us`              | Mean processing time per frame in microseconds                  |
| `timing/max_frame_us`               | Maximum processing time per frame in microseconds               |
| `upstream_rings/<ring>_count`       | Current occupancy of the upstream frame ring                    |
| `upstream_rings/<ring>_size`        | Total capacity of the upstream frame ring                       |
| `upstream_rings/clear_frames_ring_count` | Current occupancy of the clear frames buffer pool          |
| `upstream_rings/clear_frames_ring_size`  | Total capacity of the clear frames buffer pool             |
