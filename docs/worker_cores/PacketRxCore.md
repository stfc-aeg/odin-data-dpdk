## Overview

The PacketRxCore is a DPDK-based worker core that serves as the primary data ingestion point for the odin-data-dpdk system. It handles retrieving packets directly from a specified Network Interface Card (NIC), performing packet validation and protocol handling, and distributing valid data packets to downstream worker cores for further processing. Additionally, it manages the lifecycle of processed packets by handling their cleanup and release.

## Configuration

The PacketRxCore accepts the following configuration parameters through the odin-data-dpdk startup config:

```json
"packet_rx": {
   "core_name": "PacketRxCore",
   "num_cores": 1,
   "pcie_device": "0000:2c:00.0",
   "device_ip": "10.0.100.6",
   "rx_ports": [1234, 5678],
   "rx_burst_size": 128,
   "rx_queue_id": 0,
   "tx_queue_id": 0,
   "fwd_ring_size": 32768,
   "release_ring_size": 32768,
   "max_packet_tx_retries": 64,
   "max_packet_queue_retries": 64
}
```

### Configuration Parameters

| Parameter                  | Type    | Description                                                           |
| -------------------------- | ------- | --------------------------------------------------------------------- |
| `core_name`                | string  | Name of the class for this core type                                  |
| `num_cores`                | integer | Number of PacketRxCore instances to create                            |
| `pcie_device`              | string  | PCIe address of the network interface card to bind to                 |
| `device_ip`                | string  | IP address assigned to the network device (for ARP/ICMP responses)    |
| `rx_ports`                 | array   | List of UDP destination ports to accept packets from                  |
| `rx_burst_size`            | integer | Maximum number of packets to process in a single burst (default: 128) |
| `rx_queue_id`              | integer | DPDK RX queue identifier to receive packets from                      |
| `tx_queue_id`              | integer | DPDK TX queue identifier for sending reply packets                    |
| `fwd_ring_size`            | integer | Size of packet forwarding rings to downstream cores                   |
| `release_ring_size`        | integer | Size of packet release ring for cleanup                               |
| `max_packet_tx_retries`    | integer | Maximum retry attempts for transmitting reply packets                 |
| `max_packet_queue_retries` | integer | Maximum retry attempts for queueing packets to downstream cores       |

## Connections

### Upstream Connections

**None** - PacketRxCore acts as a data source, receiving packets directly from the network interface card via DPDK APIs.

### Downstream Connections

- **Packet Forward Rings**: Creates multiple rings (`packet_forward_rings_`) to distribute packets to downstream worker cores using round-robin distribution
- **Packet Release Ring**: Receives processed packets back from downstream cores for cleanup (`packet_release_ring_`)
## Runtime Configuration

The PacketRxCore supports dynamic configuration updates during runtime:

| Parameter   | Description                                   |
| ----------- | --------------------------------------------- |
| `rx_enable` | Boolean flag to enable/disable packet capture |
| `rx_frames` | Number of frames to capture (0 = unlimited)   |

When `rx_enable` is set to `false`, the frame number latch is reset, preparing for the next acquisition period.


### Packet Processing Loop

The core runs a high-performance packet processing loop in the `run()` method:

1. **Packet Reception**: Uses `rte_eth_rx_burst()` to receive packets from the NIC in configurable burst sizes
2. **Protocol Demultiplexing**: Examines Ethernet header to determine packet type
3. **Protocol Handling**: Routes packets to appropriate handlers based on protocol
4. **Packet Distribution**: Valid data packets are forwarded to downstream cores
5. **Reply Transmission**: Protocol replies (ARP, ICMP) are transmitted back to the network
6. **Cleanup**: Processes packet release requests from downstream cores


## Statistics and Monitoring

The PacketRxCore tracks several key metrics:

- `total_packets`: Total packets received from NIC
- `captured_packets`: Packets successfully forwarded to downstream cores
- `dropped_packets`: Packets discarded due to filtering or errors
- `rx_enable`: Current reception state
- `rx_frames`: Current acquisition frame limit
- `first_frame_number`: Baseline frame number for current acquisition
- `first_seen_frame_number`: Tracking variable for frame latch logic