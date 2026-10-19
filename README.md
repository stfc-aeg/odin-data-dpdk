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
  - Region of Interest cores
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
- DPDK >= 25.11

CMake 3.21 is compatible if Tensorstore functionality is not required.

## Quick Start

### Basic Installation

```bash
# Create install folder
mkdir ~/develop/install

# Clone and build odin-data
cd ~/develop/repos
git clone https://github.com/odin-detector/odin-data
cd odin-data/cpp
git checkout 1.13.0
mkdir build && cd build
cmake -DCMAKE_INSTALL_PREFIX=~/develop/install -DODINDATA_ROOT_DIR=~/develop/install ..
make -j install

# Clone and build odin-data-dpdk
cd ~/develop/repos
git clone https://github.com/stfc-aeg/odin-data-dpdk
cd odin-data-dpdk/cpp
mkdir build && cd build
cmake -DCMAKE_INSTALL_PREFIX=/develop/install -DODINDATA_ROOT_DIR=/develop/install ..
make -j install
```

### Environment Setup

On our current systems running Ubuntu 22.04.5, only CMake needs to be loaded:

```bash
module load cmake
```

On older Ubuntu versions or systems configured differently, you may also need to load Python and GCC e.g.

```bash
module load cmake
# or whatever versions you have avalible that meet the minimum requirements
```
To check what version of these you have available use `module avail`

## Usage

Run the frame processor with DPDK plugin:

```bash
cd ~/develop/install
module load dpdk
./bin/frameProcessor --ctrl tcp://127.0.0.1:5000 --log-config config/fp_log4cxx.xml --config config/<config_file_name>.json
```

## Available Plugins
Detector specific plugins can built on top of this folder structure to allow odin-data-dpdk to load them at runtime. See available plugins below:

### Packet-Based Plugins
- [mercury-detector](https://github.com/stfc-aeg/mercury-detector/tree/dpdk-plugin)
- [babyd-detector](https://github.com/stfc-aeg/babyd-detector/tree/data)
- [xidyn-detector](https://github.com/stfc-aeg/xidyn-detector)

### Camera-Based Plugins
- [odin-orca-quest](https://github.com/stfc-aeg/odin-orca-quest/tree/data)


## Tensorstore Support

Tensorstore support can be enabled at build time:

```bash
cmake -DCMAKE_INSTALL_PREFIX=/develop/install -DODINDATA_ROOT_DIR=/develop/install -DENABLE_TENSORSTORE=ON -DCMAKE_MODULE_PATH=/develop/odin-data/cpp/cmake ..
```

### Installation

Install Python tool dependencies (preferably in a virtual environment):

```bash
cd tools/python
pip install -r requirements.txt
```

## Documentation

- [Configuration Guide](docs/configuration.md)
