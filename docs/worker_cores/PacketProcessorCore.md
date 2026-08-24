## Overview

The PacketProcessorCore is a DPDK-based worker core responsible for assembling complete frames from raw network packets. It receives UDP packets forwarded by the PacketRxCore, extracts payloads, and copies them into hugepages-backed frame buffers in the shared memory buffer. When all packets for a super-frame have been received (or a configurable timeout elapses), it enqueues the assembled super-frame to downstream cores for further processing.

Multiple PacketProcessorCore instances can run in parallel; each instance is assigned a subset of frames based on its processor index, allowing horizontal scaling across NUMA-local lcores.

## Configuration

The PacketProcessorCore accepts the following configuration parameters through the odin-data-dpdk startup config:

```json
"packet_processor": {
    "core_name": "PacketProcessorCore",
    "num_cores": 4,
    "connect": "PacketRxCore",
    "upstream_core": "PacketRxCore",
    "num_downstream_cores": 1,
    "frame_timeout": 1000
}
```

### Configuration Parameters

| Parameter               | Type    | Description                                                                                  |
| ----------------------- | ------- | -------------------------------------------------------------------------------------------- |
| `core_name`             | string  | Name of the class for this core type                                                         |
| `num_cores`             | integer | Number of PacketProcessorCore instances to create                                            |
| `connect`               | string  | Name of the upstream core whose rings to connect to                                          |
| `upstream_core`         | string  | Name used to look up the upstream packet forwarding rings                                    |
| `num_downstream_cores`  | integer | Number of downstream cores to distribute assembled frames to                                 |
| `frame_timeout`         | integer | Time in milliseconds before an incomplete frame is forwarded downstream (default: 1000)      |

## Connections

### Upstream Connections

- **Packet Forward Ring** (`packet_fwd_ring_`): One ring per processor instance, named after the upstream core. Receives raw `rte_mbuf` packets from PacketRxCore.
- **Packet Release Ring** (`packet_release_ring_`): Shared ring; processed mbufs are bulk-enqueued here after their payloads have been copied, so PacketRxCore can free them.
- **Clear Frames Ring** (`clear_frames_ring_`): Shared ring pre-populated with hugepages frame buffer addresses from the DpdkSharedBuffer. The first PacketProcessorCore instance to start creates and populates this ring.

### Downstream Connections

- **Downstream Rings** (`downstream_rings_`): One ring per downstream core, named `<core_name>_<socket_id>_<ring_idx>`. Assembled `SuperFrameHeader*` buffers are enqueued here using modulo distribution based on the super-frame number.

## Runtime Configuration

The PacketProcessorCore supports dynamic configuration updates:

| Parameter      | Description                                                          |
| -------------- | -------------------------------------------------------------------- |
| `proc_enable`  | Boolean; when `true`, resets the frame number latch (`first_frame_number_` → -1), preparing for a new acquisition |

The `start_capture` command (registered with `requestCommands()`) also resets the frame number latch.

## Processing Loop

The core runs a high-performance packet processing loop in the `run()` method:

1. **Burst Dequeue**: Uses `rte_ring_dequeue_burst()` to dequeue up to 128 packets at once from the upstream packet forward ring.
2. **Frame Number Latching**: On the first packet of a new acquisition, records `first_frame_number_` to normalise all subsequent frame numbers to zero-based indices.
3. **Super-Frame Mapping**: Calculates the super-frame number and sub-frame index from the packet header via the `PacketProtocolDecoder`. Looks up or allocates a hugepages buffer for the super-frame from `clear_frames_ring_`.
4. **Payload Copy**: Uses `rte_memcpy()` to copy the packet payload into the correct offset within the super-frame buffer.
5. **Packet Accounting**: Marks each packet as received in the sub-frame header. When all packets of a sub-frame are received, marks that sub-frame complete in the super-frame header.
6. **Frame Completion**: When all sub-frames of a super-frame are complete, enqueues the buffer to the appropriate downstream ring and removes it from the in-flight map.
7. **Batch Release**: After processing the burst, bulk-enqueues all mbufs to the packet release ring for PacketRxCore to free.
8. **Timeout Handling**: Every second, iterates over the in-flight frame map and forwards any super-frame whose start time exceeds `frame_timeout` cycles to downstream cores, incrementing `incomplete_frames_`.

## Statistics and Monitoring

The PacketProcessorCore reports the following metrics under the path `<base_path>/packetprocessorcore_<idx>/`:

| Metric                       | Description                                                    |
| ---------------------------- | -------------------------------------------------------------- |
| `dropped_frames`             | Super-frames discarded because no buffer was available         |
| `dropped_packets`            | Packets discarded (tracking variable, currently not populated) |
| `frames_processed`           | Total super-frames successfully assembled and forwarded        |
| `frames_processed_per_second`| Super-frames assembled in the last second                      |
| `frames_incomplete`          | Super-frames forwarded downstream due to timeout               |
| `packets_total`              | Total packets received from the upstream ring                  |
| `frame_buffer_size`          | Number of super-frames currently in the in-flight map          |
| `idle_loops`                 | Main loop iterations where no packets were available           |
| `core_usage`                 | Core utilisation (0–255 scale, updated every second)           |
| `timing/mean_frame_us`       | Mean processing time per super-frame in microseconds           |
| `timing/max_frame_us`        | Maximum processing time per super-frame in microseconds        |
| `upstream_rings/<ring>_count`| Current occupancy of the upstream packet forward ring          |
| `upstream_rings/<ring>_size` | Total capacity of the upstream packet forward ring             |
