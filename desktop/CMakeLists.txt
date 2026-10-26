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
    GIT_TAG        release-3.4.18
    GIT_SHALLOW    TRUE
    SYSTEM  # its headers are not held to this project's warnings
)
FetchContent_Declare(inter
    URL      https://github.com/rsms/inter/releases/download/v4.1/Inter-4.1.zip
    URL_HASH SHA256=9883fdd4a49d4fb66bd8177ba6625ef9a64aa45899767dde3d36aa425756b11e
)
# Mbed TLS (Apache-2.0): ECDH P-256, AES-GCM, SHA-256 and HMAC for encrypted mode -- the same
# primitives the phone gets from the JDK. Only its crypto library is built.
set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(GEN_FILES OFF CACHE BOOL "" FORCE)
set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
set(USE_SHARED_MBEDTLS_LIBRARY OFF CACHE BOOL "" FORCE)
FetchContent_Declare(mbedtls
    URL      https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.7/mbedtls-3.6.7.tar.bz2
    URL_HASH SHA256=a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6
    SYSTEM
)
FetchContent_MakeAvailable(SDL3 inter mbedtls)

# QR codes for pairing: drawn with Nayuki's QR Code generator (MIT), read from the camera with
# quirc (ISC). Both small and plain; quirc is C.
enable_language(C)
FetchContent_Declare(qrcodegen
    URL      https://github.com/nayuki/QR-Code-generator/archive/refs/tags/v1.8.0.tar.gz
    URL_HASH SHA256=2ec0a4d33d6f521c942eeaf473d42d5fe139abcfa57d2beffe10c5cf7d34ae60
)
FetchContent_Declare(quirc
    URL      https://github.com/dlbeer/quirc/archive/refs/tags/v1.2.tar.gz
    URL_HASH SHA256=73c12ea33d337ec38fb81218c7674f57dba7ec0570bddd5c7f7a977c0deb64c5
)
# Neither has a CMakeLists of its own, so this only fetches them; the library is built below.
FetchContent_MakeAvailable(qrcodegen quirc)
# quirc 1.2 still has unbounded work on degenerate camera images; keep the small
# downstream guards explicit until upstream releases a fix (quirc issues #159/#162).
include(${CMAKE_CURRENT_SOURCE_DIR}/harden_quirc.cmake)
add_library(ff_qr STATIC
    ${qrcodegen_SOURCE_DIR}/cpp/qrcodegen.cpp
    ${quirc_SOURCE_DIR}/lib/decode.c
    ${CMAKE_CURRENT_BINARY_DIR}/quirc/identify.c
    ${CMAKE_CURRENT_BINARY_DIR}/quirc/quirc.c
    ${quirc_SOURCE_DIR}/lib/version_db.c
)
target_include_directories(ff_qr SYSTEM PUBLIC ${qrcodegen_SOURCE_DIR}/cpp ${quirc_SOURCE_DIR}/lib)
set_target_properties(ff_qr PROPERTIES CXX_STANDARD 20 POSITION_INDEPENDENT_CODE ON)
target_compile_options(ff_qr PRIVATE -w)

# Encrypted mode: pairing, sealed envelopes, teams -- the app's secure/ package in C++.
add_library(fileflow_secure STATIC src/secure.cpp)
target_include_directories(fileflow_secure PUBLIC src)
target_link_libraries(fileflow_secure PUBLIC fileflow_core PRIVATE fileflow_warnings mbedcrypto)
if(WIN32)
    target_link_libraries(fileflow_secure PRIVATE advapi32)
endif()
set_target_properties(fileflow_secure PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)

# Only explicitly configured official builds gate Pro features. Community/source builds stay open.
option(FILEFLOW_STORE_BUILD "Gate Pro features in an official distribution build" OFF)
set(FILEFLOW_LICENSE_PUBLIC_KEY_FILE "" CACHE FILEPATH "Trusted Pro signing public key (PEM SPKI; never a private key)")
set(FILEFLOW_LICENSE_PUBLIC_KEY_PEM "")
if(FILEFLOW_LICENSE_PUBLIC_KEY_FILE)
    file(READ "${FILEFLOW_LICENSE_PUBLIC_KEY_FILE}" FILEFLOW_LICENSE_PUBLIC_KEY_PEM LIMIT 4097)
    string(LENGTH "${FILEFLOW_LICENSE_PUBLIC_KEY_PEM}" FILEFLOW_LICENSE_KEY_LENGTH)
    if(FILEFLOW_LICENSE_KEY_LENGTH GREATER 4096 OR
       NOT FILEFLOW_LICENSE_PUBLIC_KEY_PEM MATCHES "^-----BEGIN PUBLIC KEY-----" OR
       FILEFLOW_LICENSE_PUBLIC_KEY_PEM MATCHES "fflicense")
        message(FATAL_ERROR "FILEFLOW_LICENSE_PUBLIC_KEY_FILE must be a public PEM SPKI key")
    endif()
endif()
configure_file(src/license_config.h.in ${CMAKE_CURRENT_BINARY_DIR}/license_config.h @ONLY)
add_library(fileflow_license STATIC src/license.cpp)
target_include_directories(fileflow_license PUBLIC src PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(fileflow_license PUBLIC fileflow_secure PRIVATE fileflow_warnings mbedcrypto)
set_target_properties(fileflow_license PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)

# Account sign-in is opt-in and separate from optical transfer. Platform TLS trusts remain intact.
set(FILEFLOW_ACCOUNT_ORIGIN "https://fileflow.anomalousinteractive.com" CACHE STRING "HTTPS account origin (no path, query or credentials)")
if(NOT FILEFLOW_ACCOUNT_ORIGIN MATCHES "^https://[A-Za-z0-9.-]+(:[0-9]+)?$" OR FILEFLOW_ACCOUNT_ORIGIN MATCHES "[\"\\\\]")
    message(FATAL_ERROR "FILEFLOW_ACCOUNT_ORIGIN must be an HTTPS origin without a path")
endif()
configure_file(src/account_config.h.in ${CMAKE_CURRENT_BINARY_DIR}/account_config.h @ONLY)
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
set(JSON_SystemInclude ON CACHE BOOL "" FORCE)
FetchContent_Declare(ff_json
    URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
    URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa
)
FetchContent_MakeAvailable(ff_json)
add_library(fileflow_account STATIC src/account.cpp)
target_include_directories(fileflow_account PUBLIC src PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(fileflow_account PUBLIC fileflow_license PRIVATE nlohmann_json::nlohmann_json fileflow_warnings)
set_target_properties(fileflow_account PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
if(APPLE)
    target_sources(fileflow_account PRIVATE src/account_http_macos.mm)
    set_source_files_properties(src/account_http_macos.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    target_link_libraries(fileflow_account PRIVATE "-framework Foundation")
    set_target_properties(fileflow_account PROPERTIES OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED ON)
elseif(WIN32)
    target_sources(fileflow_account PRIVATE src/account_http.cpp)
    target_link_libraries(fileflow_account PRIVATE winhttp)
else()
    find_package(CURL 7.85 REQUIRED)
    target_sources(fileflow_account PRIVATE src/account_http.cpp)
    target_link_libraries(fileflow_account PRIVATE CURL::libcurl)
endif()

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
    src/screen_account.cpp
    src/screen_teams.cpp
    src/camera.cpp
    src/receiver.cpp
    src/screen_receive.cpp
    src/paint.cpp
    src/transfer.cpp
    src/stb_impl.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/fonts.cpp
)
target_include_directories(fileflow-send SYSTEM PRIVATE ${STB_DIR})
# One version for the whole product: the root project's, the Android app's versionName.
target_compile_definitions(fileflow-send PRIVATE FILEFLOW_VERSION="${PROJECT_VERSION}")
target_link_libraries(fileflow-send PRIVATE fileflow_account ff_qr fileflow_warnings SDL3::SDL3-static)  # core comes with secure
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
    target_sources(fileflow-send PRIVATE third_party/nlohmann-json-LICENSE.txt)
    set_source_files_properties(third_party/nlohmann-json-LICENSE.txt PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
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
    install(FILES third_party/nlohmann-json-LICENSE.txt DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/fileflow)
    install(FILES ${ICON_DIR}/fileflow.png DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/512x512/apps)
endif()

# The desktop's own tests: its secure formats held to the phone's vectors, and the rest.
if(FILEFLOW_BUILD_TESTS)
    add_executable(quirc_limits_test tests/test_quirc_limits.c
        ${CMAKE_CURRENT_BINARY_DIR}/quirc/quirc.c ${quirc_SOURCE_DIR}/lib/version_db.c)
    target_include_directories(quirc_limits_test PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/quirc ${quirc_SOURCE_DIR}/lib)
    if(UNIX)
        target_link_libraries(quirc_limits_test PRIVATE m)
    endif()
    add_test(NAME Qr.DegenerateTransformHasBoundedWork COMMAND quirc_limits_test)
    set_tests_properties(Qr.DegenerateTransformHasBoundedWork PROPERTIES TIMEOUT 5)
    add_executable(desktop_tests tests/test_secure.cpp tests/test_qr.cpp tests/test_receive.cpp tests/test_license.cpp tests/test_account.cpp src/camera.cpp src/receiver.cpp)
    target_link_libraries(desktop_tests PRIVATE fileflow_account nlohmann_json::nlohmann_json ff_qr SDL3::SDL3-static fileflow_warnings GTest::gtest_main)
    set_target_properties(desktop_tests PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
    include(GoogleTest)
    gtest_discover_tests(desktop_tests DISCOVERY_TIMEOUT 60)
endif()
