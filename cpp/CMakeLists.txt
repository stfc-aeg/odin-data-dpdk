# Project name
project(OdinDataDpdk)

# Require CMake version >=3.0
cmake_minimum_required(VERSION 3.0)

# Set output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_CONFIG_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/test_config)

# Prepend the local cmake directory to the CMake module path to allow local finders to be used
set(CMAKE_MODULE_PATH ${OdinDataDpdk_SOURCE_DIR}/cmake ${CMAKE_MODULE_PATH})

# If the odin-data root installation directory has been specified add the cmake path to allow
# installed cmake finders to be used
if (DEFINED ODINDATA_ROOT_DIR)
	set(CMAKE_MODULE_PATH ${ODINDATA_ROOT_DIR}/cmake ${CMAKE_MODULE_PATH})
endif()

# Allow macOS shared (dynamic) libraries to link with undefined symbols
if (APPLE)
	message("Allowing undefined symbols in macOS shared library linking")
    set (CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -undefined dynamic_lookup")
endif(APPLE)

# Set up RPATH handling correctly so that both built and installed targets have the appropriate
# settings - see https://cmake.org/Wiki/CMake_RPATH_handling
set(CMAKE_MACOSX_RPATH 1)

# Use, i.e. don't skip the full RPATH for the build tree
SET(CMAKE_SKIP_BUILD_RPATH  FALSE)

# When building, don't use the install RPATH already (but later on when installing)
SET(CMAKE_BUILD_WITH_INSTALL_RPATH FALSE)

SET(CMAKE_INSTALL_RPATH "${CMAKE_INSTALL_PREFIX}/lib")

# Add the automatically determined parts of the RPATH which point to directories outside the
# build tree to the install RPATH
SET(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)

# The RPATH to be used when installing, but only if it's not a system directory
LIST(FIND CMAKE_PLATFORM_IMPLICIT_LINK_DIRECTORIES "${CMAKE_INSTALL_PREFIX}/lib" isSystemDir)
IF("${isSystemDir}" STREQUAL "-1")
   SET(CMAKE_INSTALL_RPATH "${CMAKE_INSTALL_PREFIX}/lib")
ENDIF("${isSystemDir}" STREQUAL "-1")



# Find and add external packages required for library
find_package( Boost 1.41.0
	      REQUIRED
	      COMPONENTS program_options system filesystem unit_test_framework date_time thread regex)
find_package(LOG4CXX 0.10.0 REQUIRED)
find_package(DPDK 21.1.0 REQUIRED)
find_package(Blosc)
find_package(DCAMAPI REQUIRED)

# find package HDF5
message("\nSearching for HDF5")

IF (HDF5_ROOT)
  message(STATUS "HDF5_ROOT set: " ${HDF5_ROOT})
  set( ENV{HDF5_ROOT} ${HDF5_ROOT} )
  find_path( HDF5_INCLUDE_DIRS
             NAMES hdf5.h hdf5_hl.h
             PATHS ${HDF5_ROOT}/include
             NO_DEFAULT_PATH )
  find_library( HDF5_LIBRARIES
                NAMES hdf5
                PATHS ${HDF5_ROOT}/lib
                NO_DEFAULT_PATH )
  find_library( HDF5_HL_LIBRARIES
                NAMES hdf5_hl
                PATHS ${HDF5_ROOT}/lib
                NO_DEFAULT_PATH )
  if (HDF5_HL_LIBRARIES)
    set(HDF5_FOUND true)
  else (HDF5_HL_LIBRARIES)
    set(HDF5_FOUND false)
  endif (HDF5_HL_LIBRARIES)
ELSE(HDF5_ROOT)
    find_package(HDF5 1.8.14
                 COMPONENTS C HL)
ENDIF(HDF5_ROOT)

if (HDF5_FOUND)
  message(STATUS "HDF5 include files:  " ${HDF5_INCLUDE_DIRS})
  message(STATUS "HDF5 libs:           " ${HDF5_LIBRARIES} ${HDF5_HL_LIBRARIES})
  message(STATUS "HDF5 defs:           " ${HDF5_DEFINITIONS})
ENDIF(HDF5_FOUND)


# Check if Boost version has placeholders and set definition accordingly
message("\nChecking Boost version placeholder support")
if (NOT DEFINED Boost_VERSION_STRING)
  set(Boost_VERSION_STRING "${Boost_MAJOR_VERSION}.${Boost_MINOR_VERSION}.${Boost_SUBMINOR_VERSION}")
endif ()
if ((Boost_VERSION_STRING EQUAL 1.60.0) OR (Boost_VERSION_STRING GREATER 1.60.0))
  message(STATUS "Boost version ${Boost_VERSION_STRING} has placeholders")
  ADD_DEFINITIONS(-DBOOST_HAS_PLACEHOLDERS)
  #ADD_DEFINITIONS(-DBOOST_BIND_GLOBAL_PLACEHOLDERS)
else ()
  message(STATUS "Boost version ${Boost_VERSION_STRING} does not have placeholders")
endif ()

message("Determining odin-data-dpdk version")
include(GetGitRevisionDescription)
git_describe(GIT_DESC_STR)

string(REGEX REPLACE "^([0-9]+)[\\.-].*" "\\1" VERSION_MAJOR "${GIT_DESC_STR}")
string(REGEX REPLACE "^[0-9]+[\\.-]([0-9]+).*" "\\1" VERSION_MINOR "${GIT_DESC_STR}")
string(REGEX REPLACE "^[0-9]+[\\.-][0-9]+[\\.-]([0-9]+).*" "\\1" VERSION_PATCH "${GIT_DESC_STR}")
string(REGEX REPLACE "^[0-9]+[\\.-][0-9]+[\\.-][0-9]+-[0-9]+-(.*)" "\\1" VERSION_SHA1 "${GIT_DESC_STR}")

# Resolve package version from git tag and description, write version.h include file
set(VERSION_SHORT "${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_PATCH}")
set(VERSION_STR ${GIT_DESC_STR})
message(STATUS "Git describe version: ${GIT_DESC_STR}")
message(STATUS "major:${VERSION_MAJOR} minor:${VERSION_MINOR} patch:${VERSION_PATCH} sha1:${VERSION_SHA1}")
message(STATUS "short version: ${VERSION_SHORT}")
configure_file(include/version.h.in "${CMAKE_BINARY_DIR}/include/version.h")
include_directories(${CMAKE_BINARY_DIR}/include)

# Add include directory to include path
include_directories(include)

# Add package src subdirectory
add_subdirectory(src)

# Add package include subdirectory
add_subdirectory(include)

# Add the cmake subdirectory so that CMake finders are installed
add_subdirectory(cmake)

# Add the config subdirectory
add_subdirectory(config)