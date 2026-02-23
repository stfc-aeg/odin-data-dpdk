# odin-data-dpdk

DPDK-based acceleration libraries for odin-data

## Overview

This repository extends [odin-data](https://github.com/stfc-aeg/odin-data) with DPDK-accelerated frame processing capabilities. It provides:

- **DPDK Frame Processor Plugin**: High-performance frame processing using DPDK worker cores
- **Worker Cores**: Modular processing pipeline including:
  - Packet reception cores
  - Packet processing cores
  - Frame building cores
  - Frame compression cores
  - Frame wrapper cores
  - Python access cores
- **Network and Camera Support**: Protocols and decoders for various detector types
- **TensorStore Integration**: Optional support for Tensorstore high speed writing

## Repository Structure

```
odin-data-dpdk/
├── cpp/                     # C++ source code
│   ├── src/                 # Core implementations
│   ├── include/             # Public headers
│   ├── config/              # Configuration files
│   └── CMakeLists.txt       # Build configuration
├── docs/                    # Documentation
│   ├── installation.md      # Build and installation guide
│   └── configuration.md     # Configuration examples
├── performance_testing/     # Performance testing tools
└── tools/                   # Utility scripts
```

## Prerequisites

- CMake >= 3.24
- Python >= 3.10
- GCC >= 10
- G++ >= 10
- DPDK
- odin-data (must be installed first)

## Quick Start

### Basic Installation

```bash
# Clone and build odin-data first
cd ~/develop/repos
git clone https://github.com/stfc-aeg/odin-data
cd odin-data/cpp
mkdir build && cd build
cmake -DCMAKE_INSTALL_PREFIX=~/develop/install ..
make -j install

# Clone and build odin-data-dpdk
cd ~/develop/repos
git clone https://github.com/stfc-aeg/odin-data-dpdk
cd odin-data-dpdk/cpp
mkdir build && cd build
cmake -DCMAKE_INSTALL_PREFIX=/develop/install -DODINDATA_ROOT_DIR=/develop/install -DCMAKE_MODULE_PATH=/develop/odin-data/cpp/cmake ..
make -j install
```

### Environment Setup

On Ubuntu 22.04.5, only CMake needs to be loaded (Python 3.10+ and GCC 10+ are available by default):

```bash
module load cmake/3-31-6
```

On older Ubuntu versions, you may also need to load Python and GCC :

```bash
module load python/3-11-7
module load cmake/3-31-6
export CC=/usr/bin/gcc-10
export CXX=/usr/bin/g++-10
```
To check what version of these you have available use `module avail`

## Usage

Run the frame processor with DPDK plugin:

```bash
cd ~/develop/install
module load dpdk/23.11.5
./bin/frameProcessor 
    --ctrl tcp://0.0.0.0:5000 
    --log-config config/fp_log4cxx.xml 
    --config config/<config_file_name>.json
```

## Available Plugins
Detector specific plugins can built on top of this folder structure to allow odin-data-dpdk to load them at runtime. See available plugins below:

### Packet-Based Plugins
- [mercury-detector](https://github.com/stfc-aeg/mercury-detector/tree/dpdk-plugin)
- [babyd-detector](https://github.com/stfc-aeg/babyd-detector/tree/data)

### Camera-Based Plugins
- [odin-orca-quest](https://github.com/stfc-aeg/odin-orca-quest/tree/data)


## Tensorstore Support

Tensorstore support can be enabled at build time:

```bash
cmake -DCMAKE_INSTALL_PREFIX=/develop/install -DODINDATA_ROOT_DIR=/develop/install -DENABLE_TENSORSTORE=ON -DCMAKE_MODULE_PATH=/develop/odin-data/cpp/cmake ..
```

## Documentation

- [Configuration Guide](docs/configuration.md)
