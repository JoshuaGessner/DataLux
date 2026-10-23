# core/ — the portable decode chain.
#
# INVARIANT (ADR-0010): NO Android headers, no JNI, no platform dependencies. This target
# must build for desktop with no Android toolchain present, because the simulator and the
# replay harness have to exercise THIS decoder, not a reimplementation of it.
#
# The `core-only` CMake preset and the `core-desktop` CI job exist to enforce that
# structurally: if someone adds `#include <android/...>` here, CI fails the same day.
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
)

target_include_directories(fileflow_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(fileflow_core PRIVATE fileflow_warnings)

set_target_properties(fileflow_core PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)
