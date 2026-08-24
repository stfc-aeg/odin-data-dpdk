## Overview

The FrameCompressorCore is a DPDK-based worker core that applies Blosc compression to reordered frame data before passing frames downstream. It receives `SuperFrameHeader*` buffers from an upstream core (typically FrameBuilderCore), compresses the image payload in-place using `blosc_compress_ctx()`, and updates the image size metadata in the frame header so that downstream cores know the data is compressed. The original buffer is then reused as the destination for the next frame to avoid extra allocation.

## Configuration

The FrameCompressorCore accepts the following configuration parameters through the odin-data-dpdk startup config:

```json
"frame_compressor": {
    "core_name": "FrameCompressorCore",
    "num_cores": 1,
    "connect": "FrameBuilderCore",
    "upstream_core": "FrameBuilderCore",
    "num_downstream_cores": 1,
    "dataset_name": "data",
    "blosc_clevel": 1,
    "blosc_doshuffle": 1,
    "blosc_compcode": 1,
    "blosc_blocksize": 0,
    "blosc_num_threads": 1
}
```

### Configuration Parameters

| Parameter              | Type    | Description                                                                          |
| ---------------------- | ------- | ------------------------------------------------------------------------------------ |
| `core_name`            | string  | Name of the class for this core type                                                 |
| `num_cores`            | integer | Number of FrameCompressorCore instances to create                                    |
| `connect`              | string  | Name of the upstream core whose rings to connect to                                  |
| `upstream_core`        | string  | Name used to look up the upstream frame ring                                         |
| `num_downstream_cores` | integer | Number of downstream cores to distribute compressed frames to                        |
| `dataset_name`         | string  | Name of the dataset (passed through for downstream use)                              |
| `blosc_clevel`         | integer | Blosc compression level (1–9; 1 = fastest)                                           |
| `blosc_doshuffle`      | integer | Blosc shuffle filter (0 = no shuffle, 1 = byte shuffle, 2 = bit shuffle)             |
| `blosc_compcode`       | integer | Blosc compressor code (e.g., 1 = lz4)                                               |
| `blosc_blocksize`      | integer | Blosc internal block size in bytes (0 = automatic)                                   |
| `blosc_num_threads`    | integer | Number of threads Blosc may use internally                                           |

## Connections

### Upstream Connections

- **Upstream Ring** (`upstream_ring_`): One ring per instance, named after the upstream core. Receives reordered `SuperFrameHeader*` buffers.
- **Clear Frames Ring** (`clear_frames_ring_`): Shared hugepages buffer pool. One buffer is dequeued at startup to serve as the initial compression destination (`compressed_frame_`).

### Downstream Connections

- **Downstream Rings** (`downstream_rings_`): One ring per downstream core, named `<core_name>_<socket_id>_<ring_idx>`. Compressed `SuperFrameHeader*` buffers are enqueued using `frame_number % num_downstream_cores`.

## Processing Loop

The core runs a continuous frame processing loop in the `run()` method:

1. **Frame Dequeue**: Attempts to dequeue a `SuperFrameHeader*` from the upstream ring. Increments `idle_loops_` and retries if none is available.
2. **Compression**: Calls `blosc_compress_ctx()` to compress the raw image data from the incoming buffer into the pre-allocated `compressed_frame_` buffer. The compressor, level, typesize (derived from decoder bit depth), and block size are taken from configuration.
3. **Header Copy**: Copies the super-frame and sub-frame headers from the source buffer to the compressed buffer using `rte_memcpy()`, preserving all metadata.
4. **Image Size Update**: Calls `decoder_->set_super_frame_image_size()` with the compressed size so downstream cores know the payload length.
5. **Downstream Enqueue**: Enqueues `compressed_frame_` to the appropriate downstream ring.
6. **Buffer Swap**: The just-consumed source buffer becomes the new `compressed_frame_` destination for the next iteration.

## Statistics and Monitoring

The FrameCompressorCore reports the following metrics under the path `<base_path>/FrameCompressorCore_<idx>/`:

| Metric                        | Description                                                    |
| ----------------------------- | -------------------------------------------------------------- |
| `frames_processed`            | Total frames successfully compressed and forwarded             |
| `frames_processed_per_second` | Frames compressed in the last second                           |
| `last_frame_number`           | Frame number of the most recently processed frame              |
| `idle_loops`                  | Main loop iterations where no frame was available              |
| `core_usage`                  | Core utilisation (0–255 scale, updated every second)           |
| `timing/mean_frame_us`        | Mean processing time per frame in microseconds                 |
| `timing/max_frame_us`         | Maximum processing time per frame in microseconds              |
| `upstream_rings/<ring>_count` | Current occupancy of the upstream frame ring                   |
| `upstream_rings/<ring>_size`  | Total capacity of the upstream frame ring                      |
| `upstream_rings/clear_frames_ring_count` | Current occupancy of the clear frames buffer pool   |
| `upstream_rings/clear_frames_ring_size`  | Total capacity of the clear frames buffer pool      |
