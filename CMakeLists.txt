# FileFlow — desktop entry point.
#
# ADR-0010 requires the decode chain to build and run OFF-DEVICE so the simulator and
# replay harness exercise the production decoder. This file is the desktop half of that:
# it builds core/ with no Android toolchain present. If this build ever needs an Android
# header, the architecture has been violated -- see docs/adr/ADR-0010.
#
# The Android build consumes core/CMakeLists.txt via Gradle externalNativeBuild.
cmake_minimum_required(VERSION 3.22)
project(fileflow LANGUAGES CXX VERSION 0.2.0)

set(CMAKE_CXX_STANDARD 20)          # ADR-0013: C++20, NOT C++23 (unreliable in NDK libc++)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(FILEFLOW_BUILD_TESTS "Build unit tests" ON)
option(FILEFLOW_BUILD_SIM   "Build the offline channel simulator" ON)
option(FILEFLOW_BUILD_FUZZ  "Build libFuzzer targets (needs a toolchain shipping libFuzzer)" OFF)
option(FILEFLOW_BUILD_DESKTOP "Build the desktop sender (fetches SDL3)" OFF)

# Shared with platform/android/CMakeLists.txt, which the Gradle build drives. One copy, so the
# Android build cannot quietly enforce less than CI does.
include(${CMAKE_CURRENT_LIST_DIR}/cmake/warnings.cmake)

add_subdirectory(core)

# The replay harness gates on the same option as the simulator: both are host-side tools built
# on core/, and neither is part of the shipped decoder. Declared BEFORE sim/ because ffsim
# links it for --record.
if(FILEFLOW_BUILD_SIM)
    add_subdirectory(harness)
    add_subdirectory(sim)
endif()

if(FILEFLOW_BUILD_TESTS)
    enable_testing()
    include(FetchContent)
    FetchContent_Declare(googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.17.0
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(googletest)
    add_subdirectory(core/tests)
endif()

if(FILEFLOW_BUILD_FUZZ)
    add_subdirectory(fuzz)
endif()

if(FILEFLOW_BUILD_DESKTOP)
    add_subdirectory(desktop)
endif()
