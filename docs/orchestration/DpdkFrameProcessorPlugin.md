## Overview

The DpdkFrameProcessorPlugin is the odin-data `FrameProcessorPlugin` integration point for the DPDK pipeline. It is an abstract base class that concrete detector plugins (e.g., a camera or network detector plugin) inherit from. It owns the `DpdkCoreManager` instance and bridges the standard odin-data plugin lifecycle — `configure`, `status`, `execute`, `requestCommands` — into the DPDK worker core subsystem.

On the first `configure` call the plugin constructs a `DpdkCoreManager`, which initialises the DPDK EAL, creates the shared buffer, instantiates all worker cores, and starts them on dedicated lcores. Subsequent `configure` calls with `update_config: true` are forwarded to the running manager without restarting the pipeline.

Concrete subclasses must implement `configure(config, reply)` (calling the protected overload to pass a `ProtocolDecoder*` and `FrameCallback`), and `process_frame()` to handle frames emitted by the pipeline's `FrameWrapperCore`.

## Lifecycle

```
Odin-data framework
        │
        ▼
DpdkFrameProcessorPlugin::configure(config, reply)
        │
        ├── First call ──► new DpdkCoreManager(config, ...)
        │                         └── start()   ← spawns all worker lcores
        │
        └── update_config: true ──► DpdkCoreManager::configure(config)
                                          └── forwarded to all registered cores
```

## Interface

### `configure(config, reply, decoder, frame_callback)` (protected)

Called by the concrete subclass `configure()` implementation. On the first call (or after a restart), creates a new `DpdkCoreManager`. If a manager already exists, stops and destroys it before recreating. When `update_config: true` is present in `config`, forwards the message to the running manager without restart.

| Parameter        | Description                                                             |
| ---------------- | ----------------------------------------------------------------------- |
| `config`         | IpcMessage containing startup or runtime configuration                  |
| `reply`          | IpcMessage to populate with acknowledgement or error                    |
| `decoder`        | Pointer to the `ProtocolDecoder` for the specific detector type         |
| `frame_callback` | Callback invoked by `FrameWrapperCore` when a complete frame is ready   |

### `status(status)`

Delegates to `DpdkCoreManager::status()`, which collects metrics from all running worker cores and writes them into the status IpcMessage under the plugin name path.

### `execute(command, reply)`

Delegates to `DpdkCoreManager::execute()`, which routes the command to all registered cores that declare support for it, in descending priority order.

### `requestCommands()`

Delegates to `DpdkCoreManager::requestCommands()`, which collects the union of commands from all registered cores and returns them as a list of strings for the odin-data framework to advertise.

## Version

Version information is supplied via the generated `version.h` header and exposed through `get_version_major()`, `get_version_minor()`, `get_version_patch()`, `get_version_short()`, and `get_version_long()`.
