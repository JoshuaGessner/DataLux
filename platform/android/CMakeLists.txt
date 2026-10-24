# Android native entry point — the CMakeLists Gradle's externalNativeBuild drives.
#
# WHY THIS IS NOT THE ROOT CMakeLists.txt: the root pulls GoogleTest via FetchContent and builds
# the simulator, neither of which belongs in an APK. This file adds only what ships.
#
# ADR-0013's ONE-.so RULE IS LOAD-BEARING HERE. Exactly one shared library, `libfileflow.so`,
# built with the default `c++_static`. `fileflow_core` and `fileflow_harness` are STATIC libraries
# linked into it, which does not violate the rule -- the rule is about the STL being statically
# linked into more than one *shared* object, where allocating in one and freeing in another
# corrupts the heap `[FACT]`. Adding a second .so requires switching the whole project to
# c++_shared in the same change.
cmake_minimum_required(VERSION 3.22)
project(fileflow_android LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)          # ADR-0013: C++20, NOT C++23 (unreliable in NDK libc++)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

get_filename_component(FILEFLOW_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

# Same flags CI enforces, from one file. Two copies would drift, and the drift would be silent:
# the Android build would quietly enforce less than CI does.
include(${FILEFLOW_ROOT}/cmake/warnings.cmake)

# Optimised in EVERY variant, debug included, and before the core is added so it applies there.
#
# AGP builds the debug APK's native code with CMAKE_BUILD_TYPE=Debug, which on this toolchain
# means no -O flag at all: -O0. That debug APK is the one every hardware run has used, and the
# live decoder in it took 140 ms a frame at 4080x3072 -- seven frames a second against a
# transmitter presenting fifteen -- so the phone was being measured with one hand tied (F57).
# The decode chain runs per camera frame; it has no unoptimised mode worth having. -g stays, so
# symbols and line tables are still there for a crash.
add_compile_options(-O2)

# The whole point of ADR-0010: these are the SAME sources the desktop suite and the simulator
# exercise, cross-compiled unchanged. Not a port, not a subset.
add_subdirectory(${FILEFLOW_ROOT}/core   ${CMAKE_BINARY_DIR}/core)
# harness/ carries the capture-bundle writer. ADR-0014: the recorder REUSES it rather than
# reimplementing the format, because F17's bit-identity proof is a proof about this writer.
add_subdirectory(${FILEFLOW_ROOT}/harness ${CMAKE_BINARY_DIR}/harness)

add_library(fileflow SHARED
    src/jni_probe.cpp
    src/jni_recorder.cpp
    src/jni_transmitter.cpp
    src/jni_framing.cpp
    src/jni_receiver.cpp
    src/jni_codec.cpp
)

target_link_libraries(fileflow PRIVATE
    fileflow_core
    fileflow_harness
    fileflow_warnings
    android
    log
)

set_target_properties(fileflow PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)

# Fail the build if a second shared library ever appears, rather than discovering the heap
# corruption it causes at runtime on a user's phone (ADR-0013).
get_property(_ff_targets DIRECTORY ${CMAKE_CURRENT_LIST_DIR} PROPERTY BUILDSYSTEM_TARGETS)
foreach(_t IN LISTS _ff_targets)
    get_target_property(_type ${_t} TYPE)
    if(_type STREQUAL "SHARED_LIBRARY" AND NOT _t STREQUAL "fileflow")
        message(FATAL_ERROR
            "ADR-0013: exactly one shared library is permitted with c++_static. '${_t}' is a "
            "second one. Adding it requires switching the project to c++_shared in the SAME "
            "change -- see docs/adr/ADR-0013-toolchain-and-build-system.md.")
    endif()
endforeach()
