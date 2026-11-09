cmake_minimum_required(VERSION 3.22.1)
project(hydralink LANGUAGES C CXX)

# Native core of the Hydra Link app: the network endpoint shared with Hydra's tests (src/linkcore), Hydra's own
# resampler / drift controller (src/core/Resampler.h) and libopus (external/opus), audio over AAudio.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
get_filename_component(HYDRA_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../.." ABSOLUTE)

set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
set(OPUS_DRED OFF CACHE BOOL "" FORCE)
set(OPUS_OSCE OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
add_subdirectory("${HYDRA_ROOT}/external/opus" "${CMAKE_BINARY_DIR}/opus" EXCLUDE_FROM_ALL)

add_library(hydralink SHARED
    jni.cpp
    audio.cpp
    linkcore.cpp)   # = src/linkcore/LinkLite.cpp (see there)
# shim/ first: core/RingBuffer.h's #include "Common.h" must find the stand-in, not Hydra's JUCE one.
target_include_directories(hydralink PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/shim" "${HYDRA_ROOT}/src")
target_compile_options(hydralink PRIVATE -O2 -ffast-math -Wall -Wno-unused-parameter)
target_link_libraries(hydralink PRIVATE opus aaudio log android)
