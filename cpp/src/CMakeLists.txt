set(ODINDATA_DPDK_LIBRARY "OdinDataDpdk")



include_directories(
  ${ODINDATA_ROOT_DIR}/include
  ${ODINDATA_ROOT_DIR}/include/frameProcessor
  ${CMAKE_CURRENT_SOURCE_DIR}/../include
  ${CMAKE_CURRENT_SOURCE_DIR}/../include/camera
  ${CMAKE_CURRENT_SOURCE_DIR}/../include/network
  ${DPDK_INCLUDE_DIRS}
  ${HDF5_INCLUDE_DIRS}
)

if(ENABLE_TENSORSTORE)
  include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../include/tensorstore)
endif()

set(ODINDATA_DPDK_SOURCES
        # Core DPDK files
        DpdkCoreManager.cpp
        DpdkDevice.cpp
        DpdkFrameProcessorPlugin.cpp
        DpdkSharedBuffer.cpp
        DpdkSharedBufferFrame.cpp
        DpdkUtils.cpp
        FrameBuilderCore.cpp
        FrameCompressorCore.cpp
        FrameWrapperCore.cpp
        PythonAccessCore.cpp
        
        # Camera-related
        camera/CameraCaptureCore.cpp
        camera/CameraControlCore.cpp
        camera/CameraController.cpp
        camera/CameraStateMachine.cpp
        camera/DpdkCamera.cpp
        camera/HDF5ImageLoader.cpp
        camera/SimulatedDpdkCamera.cpp
        
        # Network-related
        network/PacketProcessorCore.cpp
        network/PacketRxCore.cpp
)

# Tensorstore related
if(ENABLE_TENSORSTORE)
    list(APPEND ODINDATA_DPDK_SOURCES
        tensorstore/TensorstoreCore.cpp
        tensorstore/TensorstoreJsonSpec.cpp
        tensorstore/TensorstoreDataset.cpp
        tensorstore/TensorstoreWriter.cpp
        tensorstore/TensorstoreCSVLogger.cpp
        tensorstore/TensorstoreResizer.cpp
        tensorstore/TensorstorePerformanceMonitor.cpp
        tensorstore/TensorstoreErrorHandler.cpp
        tensorstore/TensorstoreFlushManager.cpp
    )
endif()

add_library(${ODINDATA_DPDK_LIBRARY} SHARED ${ODINDATA_DPDK_SOURCES})

target_compile_options(${ODINDATA_DPDK_LIBRARY} PRIVATE ${DPDK_CFLAGS})
target_link_directories(${ODINDATA_DPDK_LIBRARY} PRIVATE ${DPDK_LIBRARY_DIRS})
target_link_libraries(${ODINDATA_DPDK_LIBRARY} PRIVATE
    rte_eal rte_kvargs rte_telemetry
    ${DPDK_LDFLAGS} ${DCAMAPI_LIBRARY} ${HDF5_LIBRARIES} ${HDF5_HL_LIBRARIES}
)

# Add library for Tensorstore
if(ENABLE_TENSORSTORE)
    target_link_libraries(${ODINDATA_DPDK_LIBRARY} PRIVATE
        tensorstore::tensorstore tensorstore::all_drivers
    )
endif()

get_target_property(PLUGIN_INSTALL_RPATH ${ODINDATA_DPDK_LIBRARY} INSTALL_RPATH)
set_target_properties(${ODINDATA_DPDK_LIBRARY} PROPERTIES
  INSTALL_RPATH "${PLUGIN_INSTALL_RPATH}:${LIBDPDK_LIBRARY_DIRS}"
)

install(TARGETS ${ODINDATA_DPDK_LIBRARY} DESTINATION lib)

include_directories(
        ${FRAMEPROCESSOR_DIR}/include
        ${ODINDATA_INCLUDE_DIRS}
        ${ODINDATA_INCLUDE_DIR}/dpdk
        ${Boost_INCLUDE_DIRS}
        ${LOG4CXX_INCLUDE_DIRS}/..
        ${ZEROMQ_INCLUDE_DIRS}
        ${BLOSC_INCLUDE_DIR}
)

# Add library for network dummy plugin
add_library(DummyDpdkPlugin SHARED
        network/DummyDpdkPlugin.cpp
)

target_compile_options(DummyDpdkPlugin PRIVATE ${DPDK_CFLAGS})
target_link_directories(DummyDpdkPlugin PRIVATE ${ODINDATA_LIBRARY_DIR} ${DPDK_LIBRARY_DIRS})
target_link_libraries(DummyDpdkPlugin
        PRIVATE OdinDataDpdk rte_eal rte_kvargs rte_telemetry
        ${DPDK_LDFLAGS} ${BLOSC_LIBRARIES} ${HDF5_LIBRARIES} ${HDF5_HL_LIBRARIES}
)

get_target_property(PLUGIN_INSTALL_RPATH DummyDpdkPlugin INSTALL_RPATH)
set_target_properties(DummyDpdkPlugin PROPERTIES
  INSTALL_RPATH "${PLUGIN_INSTALL_RPATH}:${DPDK_LIBRARY_DIRS}"
)

install(TARGETS DummyDpdkPlugin DESTINATION lib)


# Add library for dummy camera plugin
add_library(DummyDpdkCameraPlugin SHARED
        camera/DummyDpdkCameraPlugin.cpp
)

target_compile_options(DummyDpdkCameraPlugin PRIVATE ${DPDK_CFLAGS})
target_link_directories(DummyDpdkCameraPlugin PRIVATE ${ODINDATA_LIBRARY_DIR} ${DPDK_LIBRARY_DIRS})
target_link_libraries(DummyDpdkCameraPlugin
        PRIVATE OdinDataDpdk rte_eal rte_kvargs rte_telemetry
        ${DPDK_LDFLAGS} ${BLOSC_LIBRARIES} ${HDF5_LIBRARIES} ${HDF5_HL_LIBRARIES}
)

get_target_property(PLUGIN_INSTALL_RPATH DummyDpdkCameraPlugin INSTALL_RPATH)
set_target_properties(DummyDpdkCameraPlugin PROPERTIES
  INSTALL_RPATH "${PLUGIN_INSTALL_RPATH}:${DPDK_LIBRARY_DIRS}"
)

install(TARGETS DummyDpdkCameraPlugin DESTINATION lib)
