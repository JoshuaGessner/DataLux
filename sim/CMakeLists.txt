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

# `ffframe` -- a real frame from a phone (FrameDump), compared cell by cell with what the sender
# drew: which colours are confused, where, and whether the frame's error correction survives.
add_executable(ffframe src/ffframe.cpp)
target_link_libraries(ffframe PRIVATE fileflow_sim fileflow_warnings)
set_target_properties(ffframe PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
