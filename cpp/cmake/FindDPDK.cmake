message ("\nLooking for DPDK headers and libraries")

if (DPDK_ROOT_DIR)
    message(STATUS "Searching DPDK root dir: ${DPDK_ROOT_DIR}")
endif()

find_package(PkgConfig)
if (PkgConfig_FOUND)
    message(STATUS "Using pkgconfig")
    pkg_check_modules(PC_LIBDPDK libdpdk)
endif()

find_path(LIBDPDK_INCLUDE_DIR
    NAMES
    rte_eal.h
    PATHS
    ${DPDK_ROOT_DIR}/include
    ${PC_LIBDPDK_INCLUDEDIR}
    ${PC_LIBDPDK_INCLUDE_DIRS}
)

find_library(LIBRTE_EAL_LIBRARY
    NAMES
    rte_eal
    PATHS
    ${DPDK_ROOT_DIR}/lib
    ${PC_LIBDPDK_LIBDIR}
    ${PC_LIBDPDK_LIBRARY_DIRS}
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(DPDK
    DEFAULT_MSG
    LIBRTE_EAL_LIBRARY
    LIBDPDK_INCLUDE_DIR
)

if(DPDK_FOUND)
    set(DPDK_VERSION ${PC_LIBDPDK_VERSION})
    get_filename_component(DPDK_LIBRARY_DIR ${LIBRTE_EAL_LIBRARY} PATH)
    set(DPDK_LIBRARY_DIRS ${DPDK_LIBRARY_DIR})
    set(DPDK_INCLUDE_DIRS ${DPDK_INCLUDE_DIR})
    set(DPDK_CFLAGS ${PC_LIBDPDK_CFLAGS})
    set(DPDK_LDFLAGS ${PC_LIBDPDK_LDFLAGS})

    mark_as_advanced(
        DPDK_VERSION
        DPDK_INCLUDE_DIR
        DPDK_INCLUDE_DIRS
        DPDK_LIBRARY_DIR
        DPDK_LIBRARY_DIRS
        DPDK_CFLAGS
        DPDK_LDFLAGS
    )

    message(STATUS "Found DPDK version: ${DPDK_VERSION}")
    message(STATUS "DPDK library path: ${DPDK_LIBRARY_DIR}")
    message(STATUS "DPDK include path: ${DPDK_INCLUDE_DIR}")
    message(STATUS "DPDK CFLAGS: ${DPDK_CFLAGS}")
    message(STATUS "DPDK LDFLAGS: ${DPDK_LDFLAGS}")
else()
    message(STATUS "DPDK not found, not building DPDK support libraries")
endif()