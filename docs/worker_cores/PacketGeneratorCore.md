## Overview

The PacketGeneratorCore is a DPDK-based worker core that synthesises detector-like UDP packet streams and injects them into the network pipeline. It loads a `DataSource` implementation, prepares frame data for transmission, and writes Ethernet, IPv4, and UDP headers before enqueueing each packet onto a per-device transmit ring.

## Configuration

The PacketGeneratorCore accepts the following configuration parameters through the odin-data-dpdk startup config:

```json
"packet_generator": {
    "core_name": "PacketGeneratorCore",
    "num_cores": 1,
    "stream": "primary",
    "mode": "test_16bit",
    "destination_port": 1234,
    "source_port": 2345,
    "source_ip_address": [
        "10.0.100.1",
        "10.0.100.2",
        "10.0.100.3"
    ],
    "source_mac_address": [
        "00:11:22:33:44:55",
        "00:11:22:33:44:56",
        "00:11:22:33:44:57"
    ],
    "destination_ip_address": [
        "10.0.100.4",
        "10.0.100.5",
        "10.0.100.6"
    ],
    "destination_mac_address": [
        "aa:bb:cc:dd:ee:10",
        "aa:bb:cc:dd:ee:11",
        "aa:bb:cc:dd:ee:12"
    ],
    "device_addresses": [
        "0000:00:00.0",
        "0000:00:01.0",
        "0000:00:02.0"
    ],
    "data_source_config": {
        "data_source": "generated",
        "pattern": "incrementing",
        "file_path": "/tmp/acq_1",
        "dataset_name": "/dummy"
    },
    "packet_drop": 0,
    "round_robin_mode": "frame"
}
```

### Configuration Parameters

| Parameter | Type | Description |
| --------- | ---- | ----------- |
| `core_name` | string | Name of the class for this core type |
| `num_cores` | integer | Number of PacketGeneratorCore instances to create |
| `data_source` | string | Data source type used to populate packet payloads (`generated`, `hdf5`, etc.) |
| `pattern` | string | Generated pattern used by synthetic data sources |
| `file_path` | string | File path used by file-backed data sources such as HDF5 |
| `connect` | string | Name of the upstream core whose ring is used for the core's own control path |
| `upstream_core` | string | Name used to look up upstream ring resources |
| `num_downstream_cores` | integer | Number of downstream worker cores this generator is expected to feed |
| `destination_port` | integer | UDP destination port used in generated packets |
| `source_port` | integer | UDP source port used in generated packets |
| `source_ip_address` | array | Source IPv4 addresses, one per transmit device |
| `source_mac_address` | array | Source MAC addresses, one per transmit device |
| `destination_ip_address` | array | Destination IPv4 addresses, one per transmit device |
| `destination_mac_address` | array | Destination MAC addresses, one per transmit device |
| `device_addresses` | array | PCIe addresses of DPDK devices to initialise and transmit through |
| `data_source_config` | object | Data source definition used to populate frame payloads |
| `packet_drop` | integer | Probability basis for randomly dropping generated packets |
| `round_robin_mode` | string | Distribution policy: `frame` or `packet` |
| `data_source` | string | Active data source type (for example `generated` or `hdf5`) |
| `pattern` | string | Pattern name for generated payloads |
| `file_path` | string | HDF5 file path when using an HDF5-backed data source |


## Connections

### Upstream Connections

- **Data Source**: Loads a `DataSource` implementation from `data_source_config`, which provides the raw frame data at runtime.
- **Shared Buffer Ring** (`clear_frames_ring_`): Looks up or creates the shared hugepages ring used to recycle free frame buffers from the `DpdkSharedBuffer`.
- **Control Ring** (`upstream_ring_`): Creates or looks up a ring named by the core key/processor index so the generator can participate in the worker-core topology and upstream coordination.

### Downstream Connections

- **Per-Port TX Rings**: For each DPDK port, creates or looks up a ring named `tx_port_<port_id>`, which is drained by the downstream `PacketTxCore`.
- **NIC TX Path**: Each generated packet is enqueued to the selected device ring, and the downstream TX core sends it using `rte_eth_tx_burst()`.

## Runtime Configuration

The generator supports a small runtime control surface for enabling or disabling generation:

| Parameter | Description |
| --------- | ----------- |
| `start_tx` | Enables packet generation and transmission |
| `stop_tx` | Disables packet generation without tearing down the core |

The `packet_tx_` flag gates the main loop: when `false`, the core sleeps briefly and does not generate traffic.

## Processing Loop

The core runs a high-throughput loop in `run()`:

1. **Data Fetch**: Reads the next frame from the active `DataSource`.
2. **Frame Preparation**: Calls `decoder_->prepare_frame()` to produce the encoded buffer payload for packetisation.
3. **Packet Partitioning**: Splits the frame into `packets_per_frame` slices based on the configured data width and packet size.
4. **Header Construction**: Builds the required Ethernet, IPv4, UDP, and detector packet headers for each packet.
5. **Frame Numbering**: Writes the packet and frame numbers into the packet metadata so the receiver can reconstruct ordering.
6. **Round-Robin Selection**: Chooses the target transmit device using either per-frame or per-packet round robin.
7. **MBUF Allocation**: Allocates an `rte_mbuf`, appends the packet length, and copies the payload segment into the packet buffer.
8. **Ring Enqueue**: Busy-waits until the packet is accepted onto the per-port TX ring for the downstream TX core.
