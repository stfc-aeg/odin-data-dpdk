## Overview

The FrameBuilderCore is a DPDK-based worker core responsible for reordering assembled super-frames into their final memory layout. It receives `SuperFrameHeader*` buffers from PacketProcessorCore, handles any incomplete sub-frames by zeroing dropped-packet regions, and then invokes the `PacketProtocolDecoder::reorder_frame()` method to produce a correctly ordered frame in a second hugepages buffer. The reordered buffer is then distributed to downstream cores.

Multiple FrameBuilderCore instances can run in parallel to match the number of PacketProcessorCore instances, with round-robin distribution by super-frame number.

## Configuration

The FrameBuilderCore accepts the following configuration parameters through the odin-data-dpdk startup config:

```json
"frame_builder": {
    "core_name": "FrameBuilderCore",
    "num_cores": 1,
    "connect": "PacketProcessorCore",
    "upstream_core": "PacketProcessorCore",
    "num_downstream_cores": 1
}
```

### Configuration Parameters

| Parameter              | Type    | Description                                                                   |
| ---------------------- | ------- | ----------------------------------------------------------------------------- |
| `core_name`            | string  | Name of the class for this core type                                          |
| `num_cores`            | integer | Number of FrameBuilderCore instances to create                                |
| `connect`              | string  | Name of the upstream core whose rings to connect to                           |
| `upstream_core`        | string  | Name used to look up the upstream assembled-frame ring                        |
| `num_downstream_cores` | integer | Number of downstream cores to distribute reordered frames to                  |

## Connections

### Upstream Connections

- **Upstream Ring** (`upstream_ring_`): One ring per instance, named after the upstream core. Receives assembled `SuperFrameHeader*` buffers from PacketProcessorCore.
- **Clear Frames Ring** (`clear_frames_ring_`): Shared hugepages buffer pool. The FrameBuilderCore dequeues one buffer at startup to use as its working `reordered_frame_location_`, then swaps between the incoming buffer and this location after each frame is built.

### Downstream Connections

- **Downstream Rings** (`downstream_rings_`): One ring per downstream core, named `<core_name>_<socket_id>_<ring_idx>`. Reordered `SuperFrameHeader*` buffers are enqueued using `frame_number % num_downstream_cores`.

## Processing Loop

The core runs a continuous frame processing loop in the `run()` method:

1. **Frame Dequeue**: Attempts to dequeue a `SuperFrameHeader*` from the upstream ring. Increments `idle_loops_` and retries if none is available.
2. **Incomplete Frame Handling**: Compares the number of complete sub-frames against the expected outer chunk size. For any incomplete sub-frames, iterates over all packets and zeros the memory for any dropped packets to prevent stale data corruption from buffer reuse.
3. **Frame Reorder**: Calls `decoder_->reorder_frame(current_frame_buffer_, reordered_frame_location_)` to produce the correctly ordered frame. The decoder may write into either buffer; the return value indicates which was used.
4. **Image Size Update**: Sets the total image size in the reordered frame header via `decoder_->set_super_frame_image_size()`.
5. **Downstream Enqueue**: Enqueues the reordered buffer to the appropriate downstream ring using modulo distribution.
6. **Buffer Swap**: The buffer that was just consumed becomes the new `reordered_frame_location_` for the next iteration, avoiding additional allocation.

## Statistics and Monitoring

The FrameBuilderCore reports the following metrics under the path `<base_path>/framebuildercore_<idx>/`:

| Metric                        | Description                                                    |
| ----------------------------- | -------------------------------------------------------------- |
| `frames_processed`            | Total frames successfully reordered and forwarded              |
| `frames_processed_per_second` | Frames reordered in the last second                            |
| `idle_loops`                  | Main loop iterations where no frame was available              |
| `core_usage`                  | Core utilisation (0–255 scale, updated every second)           |
| `timing/mean_frame_us`        | Mean processing time per frame in microseconds                 |
| `timing/max_frame_us`         | Maximum processing time per frame in microseconds              |
| `upstream_rings/<ring>_count` | Current occupancy of the upstream assembled-frame ring         |
| `upstream_rings/<ring>_size`  | Total capacity of the upstream assembled-frame ring            |
