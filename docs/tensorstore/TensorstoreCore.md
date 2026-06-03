## Overview

The TensorstoreCore is a DPDK-based worker core that writes frame data asynchronously to persistent storage using the [TensorStore](https://google.github.io/tensorstore/) library. It receives `SuperFrameHeader*` buffers from an upstream core and, when writing is enabled, issues asynchronous TensorStore write operations for each super-frame. Completed writes are polled each loop iteration; once a write completes the frame buffer is forwarded to downstream cores or returned to the clear frames ring.

The core supports multiple storage backends via the `kvstore_driver` parameter (e.g., local filesystem via `"file"` or S3-compatible object stores). Datasets are created or opened on each new acquisition triggered by `update_config`, with automatic capacity expansion as frames arrive. Optional CSV logging records per-write latency for performance analysis.

## Configuration

The TensorstoreCore accepts the following configuration parameters through the odin-data-dpdk startup config:

```json
"tensorstore": {
    "core_name": "TensorstoreCore",
    "num_cores": 1,
    "connect": "FrameWrapperCore",
    "upstream_core": "FrameWrapperCore",
    "num_downstream_cores": 0,
    "path": "/tmp/dataset.zarr",
    "storage_driver": "zarr",
    "kvstore_driver": "file",
    "s3_bucket": "",
    "s3_endpoint": "",
    "number_of_frames": 1000,
    "cache_bytes_limit": 10737418240,
    "max_concurrent_writes": 8,
    "delete_existing": false,
    "enable_writing": false,
    "csv_logging": false,
    "csv_path": "/tmp/tensorstore_log.csv"
}
```

### Configuration Parameters

| Parameter               | Type    | Default         | Description                                                                                  |
| ----------------------- | ------- | --------------- | -------------------------------------------------------------------------------------------- |
| `core_name`             | string  |                 | Name of the class for this core type                                                         |
| `num_cores`             | integer |                 | Number of TensorstoreCore instances to create                                                |
| `connect`               | string  |                 | Name of the upstream core whose rings to connect to                                          |
| `upstream_core`         | string  |                 | Name used to look up the upstream frame ring                                                 |
| `num_downstream_cores`  | integer |                 | Number of downstream cores (0 = return frames directly to the clear frames ring)             |
| `path`                  | string  | `"/tmp"`        | Output path for the TensorStore dataset                                                      |
| `storage_driver`        | string  |                 | TensorStore storage driver (e.g., `"zarr"`, `"n5"`)                                         |
| `kvstore_driver`        | string  | `"file"`        | TensorStore key-value store driver (`"file"` or `"s3"`)                                     |
| `s3_bucket`             | string  | `""`            | S3 bucket name (required when `kvstore_driver` is `"s3"`)                                   |
| `s3_endpoint`           | string  | `""`            | S3 endpoint URL (required when `kvstore_driver` is `"s3"`)                                  |
| `number_of_frames`      | integer | `1000`          | Expected number of frames in the acquisition (used for initial dataset allocation)           |
| `cache_bytes_limit`     | uint64  | `10737418240`   | TensorStore cache size in bytes (default: 10 GiB)                                            |
| `max_concurrent_writes` | integer |                 | Maximum number of in-flight asynchronous write operations                                    |
| `delete_existing`       | bool    | `false`         | Whether to delete an existing dataset at the configured path before creating a new one       |
| `enable_writing`        | bool    | `false`         | Whether writing is active; always starts `false` and is set `true` by `update_config`        |
| `csv_logging`           | bool    | `false`         | Enable per-write latency CSV logging                                                         |
| `csv_path`              | string  | `"/tmp"`        | Path for the CSV log file (timestamp and kvstore driver are appended per acquisition)        |

## Connections

### Upstream Connections

- **Upstream Ring** (`upstream_ring_`): One ring per instance, named after the upstream core. Receives `SuperFrameHeader*` buffers to be written.
- **Clear Frames Ring** (`clear_frames_ring_`): Shared hugepages buffer pool. Frames are returned here after their write completes, or forwarded to downstream rings if configured.

### Downstream Connections

- **Downstream Rings** (`downstream_rings_`): One ring per downstream core, named `<core_name>_<socket_id>_<ring_idx>`. Frames are forwarded here after writing completes, using `frame_number % num_downstream_cores`. If `num_downstream_cores` is 0, frames are returned directly to the `clear_frames_ring_`.

## Runtime Configuration

The TensorstoreCore supports dynamic configuration updates via `configure()`:

| Parameter               | Description                                                                                      |
| ----------------------- | ------------------------------------------------------------------------------------------------ |
| `path`                  | Output path; applied on next `update_config`                                                     |
| `storage_driver`        | TensorStore storage driver; applied on next `update_config`                                      |
| `kvstore_driver`        | Key-value store driver; applied on next `update_config`                                          |
| `max_concurrent_writes` | Maximum concurrent async writes; applied immediately                                             |
| `number_of_frames`      | Expected frame count for the new acquisition; applied on next `update_config`                    |
| `frames_per_second`     | Camera frame rate (used for CSV timestamp calculation); applied immediately                       |
| `enable_writing`        | Enable/disable writing; when set to `false`, triggers a flush of all pending writes              |
| `update_config`         | When `true`, triggers `handleReconfiguration()` — flushes pending writes, closes the current dataset, resets all counters, creates a new dataset, and enables writing for the new acquisition |

The `start_writing` and `stop_writing` commands (registered with `requestCommands()`) also toggle `enable_writing` directly.

## Processing Loop

The core runs a continuous write loop in the `run()` method:

1. **Flush Check**: If a flush has been requested (`flush_pending_writes`), waits for all pending writes to complete, forwards buffered frames, calls `TensorstoreFlushManager::FlushPendingWrites()` to trim the dataset to the last written frame, and clears the flush flag.
2. **Performance Update**: Calls `perf_monitor_.ShouldUpdate()` every second to refresh reported statistics.
3. **Completion Polling**: Calls `pollAndProcessCompletions()` to check all in-flight `WriteFutures`. For any that have completed, records timing, updates `frames_written_` / `write_errors_`, logs to CSV if enabled, and forwards the frame buffer.
4. **Back-pressure**: If the pending write queue is at `max_concurrent_writes`, skips dequeue and records an idle loop.
5. **Frame Dequeue**: Attempts to dequeue a `SuperFrameHeader*` from the upstream ring.
6. **Write Dispatch** (when writing is enabled and TensorStore is initialised):
   - Expands the dataset capacity if the incoming frame index exceeds the current allocation (increments of 1000 frames).
   - Issues `TensorstoreWriter::AsyncWriteFrameChunk()` or `AsyncWriteFrame()` for the appropriate pixel type (uint8/uint16/uint32/uint64) based on the decoder bit depth.
   - Inserts a `PendingWrite` entry into the write queue.
7. **Pass-through**: When writing is disabled or TensorStore is not initialised, forwards the frame directly without writing.

## Statistics and Monitoring

The TensorstoreCore reports the following metrics under the path `<base_path>/TensorstoreCore_<idx>/`:

| Metric                              | Description                                                         |
| ----------------------------------- | ------------------------------------------------------------------- |
| `frames_dequeued`                   | Total frames dequeued from the upstream ring                        |
| `frames_forwarded`                  | Total frames forwarded to downstream rings or clear_frames_ring     |
| `frames_processed_per_second`       | Frames processed in the last second                                 |
| `last_frame_number_dequeued`        | Super-frame number of the most recently dequeued frame              |
| `idle_loops`                        | Main loop iterations recording no work (back-pressure or idle)      |
| `core_usage`                        | Core utilisation (0–255 scale)                                      |
| `timing/mean_frame_us`              | Mean frame processing time in microseconds                          |
| `timing/max_frame_us`               | Maximum frame processing time in microseconds                       |
| `upstream_rings/<ring>_count`       | Current occupancy of the upstream frame ring                        |
| `upstream_rings/<ring>_size`        | Total capacity of the upstream frame ring                           |
| `upstream_rings/clear_frames_ring_count` | Current occupancy of the clear frames buffer pool              |
| `upstream_rings/clear_frames_ring_size`  | Total capacity of the clear frames buffer pool                 |
| `tensorstore/initialized`           | Whether a TensorStore dataset is currently open                     |
| `tensorstore/storage_path`          | Current dataset path                                                |
| `tensorstore/frames_written`        | Total frames successfully written to storage                        |
| `tensorstore/write_errors`          | Total write errors encountered                                      |
| `tensorstore/avg_write_time_us`     | Running average write latency in microseconds                       |
| `tensorstore/pending_writes_queue_size` | Number of writes currently in flight                            |
| `tensorstore/total_completed_writes`| Total write operations successfully completed                       |
| `tensorstore/enable_writing`        | Current writing enable state                                        |
| `tensorstore/last_error`            | Most recent error message (empty if no error)                       |
