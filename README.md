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
git clone https://github.com/odin-detector/odin-data
cd odin-data/cpp
git checkout 1.10.2
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

On our current systems running Ubuntu 22.04.5, only CMake needs to be loaded (Python 3.10+ and GCC 10+ are available by default):

```bash
module load cmake/3-31-6
```

On older Ubuntu versions or systems configured differently, you may also need to load Python and GCC e.g.

```bash
module load python/3-11-7
module load cmake/3-31-6
export CC=/usr/bin/gcc-10
export CXX=/usr/bin/g++-10
# or whatever versions you have avalible that meet the minimum requirements
```
To check what version of these you have available use `module avail`

## Usage

Run the frame processor with DPDK plugin:

```bash
cd ~/develop/install
module load dpdk/23.11.5
./bin/frameProcessor --ctrl tcp://0.0.0.0:5000 --log-config config/fp_log4cxx.xml --config config/<config_file_name>.json
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

NOTE: The fake cameras used to pass frames into the Tensorstore Core were modifed to enable them to output frames faster. These changes have not beenm commited as to not break other work that may be done with this repo.
```

## Python Tools

The `tools/python/` directory contains several utilities for interacting with and analyzing odin-data-dpdk:

### GUI and Control Tools

#### odin-gui.py
A GUI for controlling and monitoring odin-data-dpdk frame processors via ZMQ
```bash
python tools/python/odin-gui.py
```

#### liveviewer.py
Real-time frame visualization tool that connects to the frame processor's ZMQ stream

```bash
python tools/python/liveviewer.py --endpoint tcp://localhost:5555
```

### Data Analysis Tools

#### plot_tensorstore_performance.py
Analyzes and visualizes Tensorstore write performance from CSV logs using Matplotlib

```bash
python tools/python/plot_tensorstore_performance.py
# Opens file dialog to select CSV performance logs
```

#### TensorstoreViewer.py
Interactive viewer for Tensorstore datasets (Zarr2/Zarr3)

```bash
# View a Zarr dataset
python tools/python/TensorstoreViewer.py /path/to/dataset

# View Zarr3 dataset
python tools/python/TensorstoreViewer.py /path/to/dataset --spec zarr3
```

### Development Tools

#### frame_producer
UDP frame producer for testing and simulation. Useful for testing frame reception without a physical detector.

#### bifrost
Python library for zero-copy frame processing from odin-data-dpdk

See [bifrost/README.md](tools/python/bifrost/README.md) for detailed usage.

### Installation

Install Python tool dependencies (preferably in a virtual environment):

```bash
cd tools/python
pip install -r requirements.txt
```

## Documentation

- [Configuration Guide](docs/configuration.md)
