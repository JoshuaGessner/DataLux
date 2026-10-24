add_library(fileflow_sim STATIC
    src/channel.cpp
    src/render.cpp
    src/rendered_source.cpp
)

target_include_directories(fileflow_sim PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(fileflow_sim PUBLIC fileflow_core PRIVATE fileflow_warnings)

set_target_properties(fileflow_sim PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)

# `ffsim` — the sweep driver. Simulator runs must be reproducible from a config + commit,
# so every knob is a flag and the seed is always reported (docs/testing/SIMULATOR-PLAN.md).
add_executable(ffsim src/main.cpp)
target_link_libraries(ffsim PRIVATE fileflow_sim fileflow_harness fileflow_warnings)
set_target_properties(ffsim PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
