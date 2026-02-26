# Install headers from the main include directory
file(GLOB ODINDATA_DPDK_INCLUDES *.h)
INSTALL(FILES ${ODINDATA_DPDK_INCLUDES} DESTINATION include/dpdk)

# Install camera-related headers
file(GLOB ODINDATA_DPDK_CAMERA_INCLUDES camera/*.h)
INSTALL(FILES ${ODINDATA_DPDK_CAMERA_INCLUDES} DESTINATION include/dpdk/camera)

# Install network-related headers  
file(GLOB ODINDATA_DPDK_NETWORK_INCLUDES network/*.h)
INSTALL(FILES ${ODINDATA_DPDK_NETWORK_INCLUDES} DESTINATION include/dpdk/network)