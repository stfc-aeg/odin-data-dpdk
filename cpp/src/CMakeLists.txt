set(ODINDATA_DPDK_LIBRARY "OdinDataDpdk")
file(GLOB DPDK_LIB_SOURCES *.cpp)

include_directories(
  ${ODINDATA_ROOT_DIR}/include
  ${ODINDATA_ROOT_DIR}/include/frameProcessor
  ${DPDK_INCLUDE_DIRS}
)

add_library(${ODINDATA_DPDK_LIBRARY} SHARED 
        DpdkCoreManager.cpp
        DpdkDevice.cpp
        DpdkFrameProcessorPlugin.cpp
        DpdkSharedBuffer.cpp
        DpdkSharedBufferFrame.cpp
        DpdkUtils.cpp
        FrameBuilderCore.cpp
        FrameCompressorCore.cpp
        FrameWrapperCore.cpp
        PacketProcessorCore.cpp
        PacketRxCore.cpp
)

target_compile_options(${ODINDATA_DPDK_LIBRARY} PRIVATE ${DPDK_CFLAGS})
target_link_directories(${ODINDATA_DPDK_LIBRARY} PRIVATE ${DPDK_LIBRARY_DIRS})
target_link_libraries(${ODINDATA_DPDK_LIBRARY} PRIVATE
    rte_eal rte_kvargs rte_telemetry ${DPDK_LDFLAGS}
)

get_target_property(PLUGIN_INSTALL_RPATH ${ODINDATA_DPDK_LIBRARY} INSTALL_RPATH)

set_target_properties(${ODINDATA_DPDK_LIBRARY} PROPERTIES
  INSTALL_RPATH "${PLUGIN_INSTALL_RPATH}:${DPDK_LIBRARY_DIRS}"
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

# Add library for dummy plugin
add_library(DummyDpdkPlugin SHARED
        DummyDpdkPlugin.cpp
)

target_compile_options(DummyDpdkPlugin PRIVATE ${DPDK_CFLAGS})
target_link_directories(DummyDpdkPlugin PRIVATE ${ODINDATA_LIBRARY_DIR} ${DPDK_LIBRARY_DIRS})
target_link_libraries(DummyDpdkPlugin
        PRIVATE OdinDataDpdk rte_eal rte_kvargs rte_telemetry
        ${DPDK_LDFLAGS} ${BLOSC_LIBRARIES}
)

get_target_property(PLUGIN_INSTALL_RPATH DummyDpdkPlugin INSTALL_RPATH)


set_target_properties(DummyDpdkPlugin PROPERTIES
  INSTALL_RPATH "${PLUGIN_INSTALL_RPATH}:${DPDK_LIBRARY_DIRS}"
)

install(TARGETS DummyDpdkPlugin DESTINATION lib)
