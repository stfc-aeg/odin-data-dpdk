# Installation

Create the folder structure for the installation:
``` bash
cd ~/develop/projects/daq
mkdir install && repos
cd repos
```

Clone the odin-data repo:
``` bash
git clone https://github.com/stfc-aeg/odin-data
```

Then build odin-data, more detailed instrcutions can be found [here](https://odin-detector.github.io/odin-data/master/user/tutorials/build.html) in the odin-data documentation:

```bash
cd odin-data/cpp
mkdir build && cd build
cmake -DCMAKE_INSTALL_PREFIX=~/develop/projects/daq/install ..
make -j install
```

You can test this installation by running any of the unit test applications:

```bash
~/develop/projects/daq/install/bin/frameProcessorTest
```

It's now time to build odin-data-dpdk, clone the repo:

``` bash
cd ~/develop/projects/daq/repos/
git clone https://github.com/stfc-aeg/odin-data-dpdk
```

Then create the build folder and confgiure the project with cmake:

```bash
cd odin-data-dpdk/cpp
mkdir build && cd build
cmake -DCMAKE_INSTALL_PREFIX=~/develop/projects/daq/install -ODINDATA_ROOT_DIR=~/develop/projects/daq/install ..
make -j install
```

Move the to the install folder and try running one of the example plugins:

```bash
cd ~/develop/projects/daq/repos/install
./bin/frameProcessor --ctrl tcp://0.0.0.0:5000 --log-config config/data/fp_log4cxx.xml --config config/data/DummyExampleCamera.json
```

Detector sepcific plugins can then be built on this of this folder structure to allow odin-data-dpdk to load them at runtime. See available plugins below:

## Packet based plugins
https://github.com/stfc-aeg/mercury-detector/tree/dpdk-plugin
https://github.com/stfc-aeg/babyd-detector/tree/data

## Camera based plugins
https://github.com/stfc-aeg/odin-orca-quest/tree/data
