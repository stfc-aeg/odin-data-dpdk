## Overview

The FrameWrapperCore is the final stage of the DPDK worker core pipeline. It receives `SuperFrameHeader*` buffers from an upstream core, wraps them into a `DpdkSharedBufferFrame` object (a `boost::shared_ptr<Frame>`), and delivers them to the odin-data plugin chain via the `FrameCallback`. This is the bridge between the zero-copy hugepages pipeline and the standard odin-data frame processing interface.

The FrameWrapperCore detects whether a frame has been Blosc-compressed by comparing the stored image size against the expected uncompressed size, and sets the compression type on the frame metadata accordingly.

## Configuration

The FrameWrapperCore accepts the following configuration parameters through the odin-data-dpdk startup config:

```json
"frame_wrapper": {
    "core_name": "FrameWrapperCore",
    "num_cores": 1,
    "connect": "FrameCompressorCore",
    "upstream_core": "FrameCompressorCore",
    "num_downstream_cores": 0,
    "dataset_name": "data",
    "blosc_clevel": 1,
    "blosc_doshuffle": 1,
    "blosc_compcode": 1,
    "blosc_blocksize": 0,
    "blosc_num_threads": 1
}
```

### Configuration Parameters

| Parameter              | Type    | Description                                                                                |
| ---------------------- | ------- | ------------------------------------------------------------------------------------------ |
| `core_name`            | string  | Name of the class for this core type                                                       |
| `num_cores`            | integer | Number of FrameWrapperCore instances to create                                             |
| `connect`              | string  | Name of the upstream core whose rings to connect to                                        |
| `upstream_core`        | string  | Name used to look up the upstream frame ring                                               |
| `num_downstream_cores` | integer | Number of downstream cores (typically 0; this is the pipeline terminus)                    |
| `dataset_name`         | string  | Dataset name written into the `FrameMetaData` for each frame                              |
| `blosc_clevel`         | integer | Blosc compression level (carried in config but not actively used here)                     |
| `blosc_doshuffle`      | integer | Blosc shuffle setting (carried in config but not actively used here)                       |
| `blosc_compcode`       | integer | Blosc compressor code (carried in config but not actively used here)                       |
| `blosc_blocksize`      | integer | Blosc block size (carried in config but not actively used here)                            |
| `blosc_num_threads`    | integer | Blosc thread count (carried in config but not actively used here)                          |

## Connections

### Upstream Connections

- **Upstream Ring** (`upstream_ring_`): One ring per instance, named after the upstream core. Receives `SuperFrameHeader*` buffers ready for delivery.
- **Clear Frames Ring** (`clear_frames_ring_`): Shared hugepages buffer pool. The `DpdkSharedBufferFrame` holds a reference to this ring and returns the hugepages buffer to it when the shared pointer is released by the plugin chain.

### Downstream Connections

**None** — the FrameWrapperCore is the pipeline terminus. Frames are delivered to odin-data via `frame_callback_` rather than to another worker ring. The hugepages buffer is returned to the `clear_frames_ring_` automatically when the `DpdkSharedBufferFrame` shared pointer is destroyed.

## Processing Loop

The core runs a continuous frame wrapping loop in the `run()` method:

1. **Frame Dequeue**: Attempts to dequeue a `SuperFrameHeader*` from the upstream ring. Increments `idle_loops` and retries if none is available.
2. **Dimension Query**: On startup, retrieves frame dimensions from the decoder and calculates the expected uncompressed frame size.
3. **Metadata Construction**: Creates a `FrameMetaData` object populated with the dataset name, frame number, dimensions, and data type from the decoder.
4. **Compression Detection**: Reads the image size from the super-frame header. If it differs from the expected uncompressed size, sets compression type to `blosc`; otherwise sets `no_compression`.
5. **Frame Wrapping**: Creates a `DpdkSharedBufferFrame` `boost::shared_ptr<Frame>` wrapping the hugepages buffer. The frame holds a reference to the `clear_frames_ring_` so the memory is automatically reclaimed when the shared pointer goes out of scope in the plugin chain.
6. **Callback Delivery**: Sets the image size and outer chunk size on the frame, then invokes `frame_callback_(complete_frame)` to deliver the frame to the odin-data plugin chain.

## Statistics and Monitoring

The FrameWrapperCore reports the following metrics under the path `<base_path>/FrameWrapperCore_<idx>/`:

| Metric                        | Description                                                    |
| ----------------------------- | -------------------------------------------------------------- |
| `frames_processed`            | Total frames wrapped and delivered to the plugin chain         |
| `frames_processed_per_second` | Frames wrapped in the last second                              |
| `last_frame_number`           | Frame number of the most recently wrapped frame                |
| `idle_loops`                  | Main loop iterations where no frame was available              |
| `core_usage`                  | Core utilisation (0–255 scale, updated every second)           |
| `timing/mean_frame_us`        | Mean processing time per frame in microseconds                 |
| `timing/max_frame_us`         | Maximum processing time per frame in microseconds              |
| `upstream_rings/<ring>_count` | Current occupancy of the upstream frame ring                   |
| `upstream_rings/<ring>_size`  | Total capacity of the upstream frame ring                      |
| `upstream_rings/clear_frames_ring_count` | Current occupancy of the clear frames buffer pool   |
| `upstream_rings/clear_frames_ring_size`  | Total capacity of the clear frames buffer pool      |
