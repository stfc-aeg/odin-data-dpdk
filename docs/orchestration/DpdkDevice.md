## Overview

The DpdkDevice class encapsulates the initialisation and lifecycle management of a single DPDK Ethernet port. It is constructed by the `PacketRxCore` (or equivalent network ingress core) and handles the low-level setup required before any packets can be received or transmitted: mbuf pool creation, port configuration, queue setup, and starting the device in promiscuous mode.

Each `DpdkDevice` instance corresponds to one physical or virtual NIC port identified by its DPDK port ID.

## Configuration

The DpdkDevice is configured via a `DpdkDeviceConfiguration` object, which is typically populated from the `packet_rx` section of the startup config:

```json
"packet_rx": {
    ...
    "mbuf_pool_size": 500000,
    "mbuf_cache_size": 512,
    "mtu": 9600,
    "rx_rings": 1,
    "rx_num_desc": 16384,
    "tx_rings": 1,
    "tx_num_desc": 8192
}
```

### Configuration Parameters

| Parameter        | Type     | Default  | Description                                                                          |
| ---------------- | -------- | -------- | ------------------------------------------------------------------------------------ |
| `mbuf_pool_size` | integer  | 500000   | Number of mbufs to allocate in the packet buffer pool                                |
| `mbuf_cache_size`| integer  | 512      | Per-lcore mbuf cache size for the pool                                               |
| `mtu`            | uint32   | 9600     | Maximum transmission unit in bytes; set as the RX mode MTU and the mbuf data size   |
| `rx_rings`       | uint16   | 1        | Number of RX queues to configure on the device                                       |
| `rx_num_desc`    | uint16   | 16384    | Number of descriptors per RX queue ring                                              |
| `tx_rings`       | uint16   | 1        | Number of TX queues to configure on the device                                       |
| `tx_num_desc`    | uint16   | 8192     | Number of descriptors per TX queue ring                                              |

## Initialisation Sequence

The constructor performs all device setup in order:

1. **Device identification**: Calls `rte_eth_dev_get_name_by_port()` and `rte_eth_macaddr_get()` to retrieve and log the PCI device name and MAC address.
2. **mbuf pool creation** (`init_mbuf_pool`): Creates a packet mbuf pool via `rte_pktmbuf_pool_create()` named `mbuf_pool_<socket_id>`. If the pool already exists (e.g., in a secondary DPDK process), it is looked up rather than recreated.
3. **Port configuration** (`init_port`):
   - Retrieves device capabilities via `rte_eth_dev_info_get()`.
   - Configures the port with `rte_eth_dev_configure()`, setting the RX MTU and enabling hardware offloads if available:
     - **TX fast-free** (`RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE`): reduces TX path overhead.
     - **RX scatter** (`RTE_ETH_RX_OFFLOAD_SCATTER`): enables reception of jumbo frames larger than a single mbuf.
   - Adjusts descriptor counts to device-supported values via `rte_eth_dev_adjust_nb_rx_tx_desc()`.
   - Sets up one RX queue and one TX queue with the configured descriptor counts.

## Lifecycle

| Method    | Description                                                                              |
| --------- | ---------------------------------------------------------------------------------------- |
| `start()` | Calls `rte_eth_dev_start()` to bring the port up, then enables promiscuous mode          |
| `stop()`  | Calls `rte_eth_dev_stop()` to take the port down                                         |

The destructor calls `stop()`. The `DpdkCoreManager` destructor additionally calls `rte_eth_dev_close()` on all valid ports after stopping them.

## Accessors

| Method       | Returns                                      |
| ------------ | -------------------------------------------- |
| `port_id()`  | The DPDK port identifier (uint16_t)          |
| `socket_id()`| The NUMA socket the port is associated with  |
