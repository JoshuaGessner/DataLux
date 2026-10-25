# core/ — the portable decode chain.
#
# INVARIANT (ADR-0010): NO Android headers, no JNI, no platform dependencies. This target
# must build for desktop with no Android toolchain present, because the simulator and the
# replay harness have to exercise THIS decoder, not a reimplementation of it.
#
# The `core-only` CMake preset and the `core-desktop` CI job exist to enforce that
# structurally: if someone adds `#include <android/...>` here, CI fails the same day.
# Zstandard (BSD) for file compression: portable C, no platform dependency, so it keeps the
# invariant above. Pinned to a release by hash; built static, so the APK still carries exactly one
# .so (ADR-0013).
include(FetchContent)
# Extracted files dated now, not from the archive (CMake 3.24+; the Android SDK's 3.22 predates it).
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()
set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ZSTD_LEGACY_SUPPORT OFF CACHE BOOL "" FORCE)
set(ZSTD_MULTITHREAD_SUPPORT OFF CACHE BOOL "" FORCE)
FetchContent_Declare(zstd
    URL https://github.com/facebook/zstd/releases/download/v1.5.7/zstd-1.5.7.tar.gz
    URL_HASH SHA256=eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3
    SOURCE_SUBDIR build/cmake
)
FetchContent_MakeAvailable(zstd)
set_target_properties(libzstd_static PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(fileflow_core STATIC
    src/result.cpp
    src/hash.cpp
    src/gf256.cpp
    src/fountain.cpp
    src/grid.cpp
    src/frame.cpp
    src/modulation.cpp
    src/transfer.cpp
    src/session.cpp
    src/live.cpp
    src/geometry.cpp
    src/sampler.cpp
    src/photometric.cpp
    src/detect.cpp
    src/tracker.cpp
    src/pipeline.cpp
    src/intra_fec.cpp
    src/link.cpp
    src/device.cpp
    src/framing.cpp
    src/compress.cpp
    src/envelope.cpp
)

target_include_directories(fileflow_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(fileflow_core PRIVATE fileflow_warnings libzstd_static)
target_include_directories(fileflow_core PRIVATE ${zstd_SOURCE_DIR}/lib)

set_target_properties(fileflow_core PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)
