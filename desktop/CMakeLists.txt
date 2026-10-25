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
    src/app.cpp
    src/ui.cpp
    src/screen_send.cpp
    src/paint.cpp
    src/transfer.cpp
    src/stb_impl.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/fonts.cpp
)
target_include_directories(fileflow-send SYSTEM PRIVATE ${STB_DIR})
# One version for the whole product: the root project's, the Android app's versionName.
target_compile_definitions(fileflow-send PRIVATE FILEFLOW_VERSION="${PROJECT_VERSION}")
target_link_libraries(fileflow-send PRIVATE fileflow_core fileflow_warnings SDL3::SDL3-static)
# Third-party and generated sources: not held to the project's warnings.
set_source_files_properties(src/stb_impl.cpp ${CMAKE_CURRENT_BINARY_DIR}/fonts.cpp PROPERTIES COMPILE_OPTIONS "-w")
set_target_properties(fileflow-send PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)

# The icon, drawn by a small tool at build time at every size each platform asks for.
add_executable(ff-icon tools/icon.cpp)
target_link_libraries(ff-icon PRIVATE fileflow_warnings)
set_target_properties(ff-icon PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
set(ICON_DIR ${CMAKE_CURRENT_BINARY_DIR}/icon)
set(ICONSET ${ICON_DIR}/FileFlow.iconset)
add_custom_command(
    OUTPUT ${ICON_DIR}/FileFlow.ico ${ICON_DIR}/fileflow.png ${ICONSET}/icon_512x512@2x.png
    COMMAND ff-icon ${ICONSET} ${ICON_DIR}/FileFlow.ico ${ICON_DIR}/fileflow.png
    DEPENDS ff-icon
    COMMENT "Drawing the icon"
    VERBATIM)

if(APPLE)
    # FileFlow.app: double-clickable, its icon in the Dock, files dropped on that icon sent, and the
    # camera permission text macOS shows before the camera is used.
    add_custom_command(
        OUTPUT ${ICON_DIR}/FileFlow.icns
        COMMAND iconutil -c icns ${ICONSET} -o ${ICON_DIR}/FileFlow.icns
        DEPENDS ${ICONSET}/icon_512x512@2x.png
        COMMENT "Packing the icon for macOS"
        VERBATIM)
    target_sources(fileflow-send PRIVATE ${ICON_DIR}/FileFlow.icns)
    set_source_files_properties(${ICON_DIR}/FileFlow.icns PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    set_target_properties(fileflow-send PROPERTIES
        MACOSX_BUNDLE TRUE
        OUTPUT_NAME FileFlow
        MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/macos/Info.plist.in
        MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
        MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION})
    # Signed as a whole (ad hoc: no developer certificate needed) under its bundle id. The linker
    # signs only the bare binary; macOS remembers a camera permission against the bundle's signature,
    # and without one it asks every launch or not at all.
    add_custom_command(TARGET fileflow-send POST_BUILD
        COMMAND codesign --force --sign - --identifier dev.fileflow.desktop $<TARGET_BUNDLE_DIR:fileflow-send>
        COMMENT "Signing FileFlow.app (ad hoc)"
        VERBATIM)
elseif(WIN32)
    # The icon in Explorer and the taskbar; a window app, not a console one.
    set(FILEFLOW_ICO ${ICON_DIR}/FileFlow.ico)
    configure_file(windows/fileflow.rc.in ${CMAKE_CURRENT_BINARY_DIR}/fileflow.rc @ONLY)
    target_sources(fileflow-send PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/fileflow.rc ${ICON_DIR}/FileFlow.ico)
    set_target_properties(fileflow-send PROPERTIES WIN32_EXECUTABLE TRUE OUTPUT_NAME FileFlow)
else()
    # Linux: the program, its menu entry and its icon, for `cmake --install`.
    add_custom_target(fileflow-icon ALL DEPENDS ${ICON_DIR}/fileflow.png)
    include(GNUInstallDirs)
    install(TARGETS fileflow-send RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(FILES linux/fileflow.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    install(FILES ${ICON_DIR}/fileflow.png DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/512x512/apps)
endif()
