# desktop/ — FileFlow for Windows, macOS and Linux: a laptop, monitor, TV or projector as the
# sending screen.
#
# A thin shell around core/, as the Android app is: core makes every frame, this only shows them.
# SDL3 (zlib licence) gives one window, one vsync'd renderer, file drag-and-drop and the system's
# file dialog on all three systems, so there is one shell to keep in step with the phone, not
# three. Text is Inter (SIL OFL 1.1) drawn with stb_truetype (public domain), the font embedded in
# the program. Everything is fetched pinned -- by tag or by hash -- and built static, so the result
# is one executable with nothing to install.
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
FetchContent_Declare(inter
    URL      https://github.com/rsms/inter/releases/download/v4.1/Inter-4.1.zip
    URL_HASH SHA256=9883fdd4a49d4fb66bd8177ba6625ef9a64aa45899767dde3d36aa425756b11e
)
FetchContent_MakeAvailable(SDL3 inter)

# stb_truetype v1.26, pinned to its commit.
set(STB_DIR ${CMAKE_CURRENT_BINARY_DIR}/third_party)
if(NOT EXISTS ${STB_DIR}/stb_truetype.h)
    file(DOWNLOAD
        https://raw.githubusercontent.com/nothings/stb/6e9f34d5429cf16790ec43c9bac3f1ee4ad1f760/stb_truetype.h
        ${STB_DIR}/stb_truetype.h
        EXPECTED_HASH SHA256=ecd30b05e0dd4fea3a13c26810dd9e1992dc379049482c393d5a19e6b5090aab)
endif()

# The two weights the app uses, as byte arrays in a generated source.
set(FONT_REGULAR ${inter_SOURCE_DIR}/extras/ttf/Inter-Regular.ttf)
set(FONT_SEMIBOLD ${inter_SOURCE_DIR}/extras/ttf/Inter-SemiBold.ttf)
set(FONT_FILES ${FONT_REGULAR} ${FONT_SEMIBOLD})
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/fonts.cpp
    COMMAND ${CMAKE_COMMAND} -DOUT=${CMAKE_CURRENT_BINARY_DIR}/fonts.cpp
            -DNAMES=kInterRegular,kInterSemiBold -DFILES=${FONT_REGULAR},${FONT_SEMIBOLD}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/embed.cmake
    DEPENDS ${FONT_FILES} ${CMAKE_CURRENT_SOURCE_DIR}/embed.cmake
    COMMENT "Embedding Inter"
    VERBATIM)

add_executable(fileflow-send
    src/main.cpp
    src/paint.cpp
    src/transfer.cpp
    src/stb_impl.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/fonts.cpp
)
target_include_directories(fileflow-send SYSTEM PRIVATE ${STB_DIR})
target_link_libraries(fileflow-send PRIVATE fileflow_core fileflow_warnings SDL3::SDL3-static)
# Third-party and generated sources: not held to the project's warnings.
set_source_files_properties(src/stb_impl.cpp ${CMAKE_CURRENT_BINARY_DIR}/fonts.cpp PROPERTIES COMPILE_OPTIONS "-w")
set_target_properties(fileflow-send PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)
