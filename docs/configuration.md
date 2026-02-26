# Configuration files for odin-data-dpdk

Below is an example section of valid config for the DummyDpdk plugin, which is a odin-data-dpdk plugin always built with this project. The config provided to odin-data-dpdk is made up of two main sections, [TODO] and the second section, which is parsed and provided to the running worker cores.

``` json

"DummyDpdk": {
    "update_config": false,
    "dpdk_process_rank": 0,
    "num_secondary_processes": 0,
    "shared_buffer_size": 17179869184,
    "dpdk_eal": {
        "corelist": "0,2,4,6,8,10,12,14,16,18,20,22,24,26,28,30",
        "loglevel": "debug",
        "proc-type": "Primary",
        "file-prefix": "odin-data"
    },
    "worker_cores": {
        "packet_rx": {
            "core_name": "PacketRxCore",
            "num_cores": 1,
            "pcie_device": "0000:2c:00.0",
            "rx_ports": [
                1234
            ],
            "device_ip": "10.0.100.6",
            "rx_burst_size": 128,
            "fwd_ring_size": 32786,
            "release_ring_size": 32768,
            "rx_queue_id": 0,
            "tx_queue_id": 0,
            "max_packet_tx_retries": 64,
            "max_packet_queue_retries": 64
        },
        "packet_processor": {
            "core_name": "PacketProcessorCore",
            "num_cores": 6,
            "connect": "packet_rx",
            "frame_timeout": 1000
        },
        "frame_builder": {
            "core_name": "FrameBuilderCore",
            "num_cores": 4,
            "connect": "packet_processor"
        },
        "frame_wrapper": {
            "core_name": "FrameWrapperCore",
            "dataset_name": "dummy",
            "num_cores": 1,
            "connect": "frame_builder"
        }
    }
}

```