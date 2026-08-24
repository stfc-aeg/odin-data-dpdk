## Overview

The DpdkCoreManager is the central orchestration class for the odin-data-dpdk pipeline. It is responsible for:

1. **DPDK EAL initialisation** — initialises the DPDK Environment Abstraction Layer using parameters parsed from the startup configuration, redirecting DPDK log output into the odin-data log4cxx logger.
2. **Core chain resolution** — parses the `worker_cores` configuration block to infer which core connects to which, automatically injecting `upstream_core` and `num_downstream_cores` fields so each worker core knows where to find its rings.
3. **Shared buffer allocation** — creates the `DpdkSharedBuffer` hugepages pool shared by all worker cores for zero-copy frame storage.
4. **Worker core instantiation** — uses `DpdkCoreLoader` to dynamically load and construct each worker core class by name, passing a shared `DpdkWorkCoreReferences` struct containing the configuration, decoder, frame callback, and shared buffer.
5. **NUMA-aware lcore scheduling** — assigns each worker core to an available worker lcore on the correct NUMA socket using `rte_eal_remote_launch`.
6. **Runtime command dispatch** — collects and routes `execute()` commands to registered worker cores by declared priority.

## Configuration

The DpdkCoreManager is configured from the top-level startup IpcMessage. All worker core configuration lives under the `worker_cores` key, with each sub-key naming a core group.

```json
{
    "dpdk_eal": {
        "corelist": "0-7",
        "allow": "0000:2c:00.0",
        "loglevel": "7",
        "proc-type": "primary",
        "file-prefix": "odin"
    },
    "shared_buffer_size": 8589934592,
    "socket": 0,
    "num_secondary_processes": 0,
    "dpdk_process_rank": 0,
    "worker_cores": {
        "packet_rx": { "core_name": "PacketRxCore", "num_cores": 1, ... },
        "packet_processor": { "core_name": "PacketProcessorCore", "num_cores": 4, "connect": "packet_rx", ... },
        "frame_builder": { "core_name": "FrameBuilderCore", "num_cores": 1, "connect": "packet_processor", ... },
        "frame_wrapper": { "core_name": "FrameWrapperCore", "num_cores": 1, "connect": "frame_builder", ... }
    }
}
```

### Top-level Configuration Parameters

| Parameter               | Type    | Default        | Description                                                                                   |
| ----------------------- | ------- | -------------- | --------------------------------------------------------------------------------------------- |
| `shared_buffer_size`    | size_t  | 8589934592     | Total hugepages buffer pool size in bytes (default: 8 GiB)                                    |
| `socket`                | integer | 0              | NUMA socket ID to allocate the shared buffer on                                               |
| `num_secondary_processes` | integer | 0            | Number of secondary DPDK processes sharing the same EAL resources                            |
| `dpdk_process_rank`     | integer | 0              | Rank of this DPDK process; used to offset per-core instance indices in multi-process setups   |

### DPDK EAL Parameters (`dpdk_eal`)

| Parameter      | DPDK flag       | Description                                              |
| -------------- | --------------- | -------------------------------------------------------- |
| `corelist`     | `-l`            | Comma-separated list or range of lcores to use           |
| `allow`        | `--allow`       | PCIe device to allow (can be repeated as an array)       |
| `loglevel`     | `--log-level`   | DPDK log verbosity level                                 |
| `proc-type`    | `--proc-type`   | DPDK process type (`primary` or `secondary`)             |
| `file-prefix`  | `--file-prefix` | Shared memory file prefix (required for multi-process)   |

### Worker Core Chain (`worker_cores`)

Each key in `worker_cores` names a core group. The value is the per-core configuration object documented in each worker core's own page. The `connect` field within a core group's config identifies the JSON key of the upstream core group. The DpdkCoreManager uses this to:

- Inject `upstream_core` (the `core_name` string of the upstream group) into each downstream core's config.
- Inject `num_downstream_cores` (the downstream group's `num_cores`, multiplied by `num_secondary_processes + 1` if the upstream core has `secondary_fanout: true`) into each upstream core's config.

This means `upstream_core` and `num_downstream_cores` do **not** need to be specified manually in the config file — they are resolved automatically.

## Startup Sequence

```
DpdkCoreManager constructor
    │
    ├── rte_eal_init()           ← DPDK EAL init with parsed EAL args
    │
    ├── Map worker lcores to NUMA sockets
    │
    ├── Parse worker_cores block
    │   ├── Build core_chain_order_ bimap (upstream ↔ downstream JSON keys)
    │   └── Inject upstream_core / num_downstream_cores into each core's config
    │
    ├── new DpdkSharedBuffer(shared_buffer_size, frame_buffer_size, socket)
    │
    └── For each core group (in config order):
        └── For i in [0, num_cores):
            └── DpdkCoreLoader::load_class(core_name, i + process_offset, socket, refs)
                └── register_worker_core(core)

DpdkCoreManager::start()
    │
    ├── For each registered core: core->connect()
    │
    └── For each registered core:
        ├── Find next unused lcore on the requested NUMA socket
        └── rte_eal_remote_launch(start_worker, core, lcore_id)
```

## Command Dispatch

Worker cores can register named commands via `requestCommands()`, each with an integer priority. The DpdkCoreManager:

- **`requestCommands()`** — collects commands from all registered cores; for duplicate command names, retains the highest declared priority.
- **`execute(command, reply)`** — finds all cores that declare the command, sorts them by descending priority, and calls `core->execute()` on each in order.

This allows a single command (e.g., `start_capture`) to trigger coordinated action across multiple core types at defined relative orderings.

## Status Reporting

`status(status)` writes `shared_buffer_size` under `<plugin_name>/core_manager/` and then delegates to `core->status()` for each running core, which writes its own metrics under `<plugin_name>/<core_type>_<idx>/`.

## Shutdown

`stop()` calls `core->stop()` on each running core, waits for the lcore to complete via `rte_eal_wait_lcore()`, removes the lcore from the used list, waits 1 ms for pending operations, then resets and clears all `shared_ptr` instances. Shared buffers are deleted and all active DPDK ethernet ports are stopped and closed in the destructor.
