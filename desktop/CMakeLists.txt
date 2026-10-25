# desktop/ — FileFlow for Windows, macOS and Linux: a laptop, monitor, TV or projector as the
# sending screen.
#
# A thin shell around core/, as the Android app is: core makes every frame, this only shows them.
# SDL3 (zlib licence) gives one window, one vsync'd renderer and file drag-and-drop on all three
# systems, so there is one shell to keep in step with the phone, not three. Built static, so the
# result is one executable with nothing to install.
#
# CMake 3.25 for FetchContent's SYSTEM; the Android build, held at 3.22, never comes here.
cmake_minimum_required(VERSION 3.25)
include(FetchContent)
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG        release-3.2.10
    GIT_SHALLOW    TRUE
    SYSTEM  # its headers are not held to this project's warnings
)
FetchContent_MakeAvailable(SDL3)

add_executable(fileflow-send src/main.cpp)
target_link_libraries(fileflow-send PRIVATE fileflow_core fileflow_warnings SDL3::SDL3-static)
set_target_properties(fileflow-send PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    MACOSX_BUNDLE OFF
)
