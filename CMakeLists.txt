cmake_minimum_required(VERSION 3.21)
project(osect C CXX)

# Package metadata (used by CPack on both platforms and by the macOS bundle)
set(APPLICATION_NAME "OpenSectional")
set(MAJOR_VERSION "0")
set(MINOR_VERSION "1")
set(BUILD_VERSION "0")
set(APPLICATION_VERSION "${MAJOR_VERSION}.${MINOR_VERSION}.${BUILD_VERSION}")
set(BUNDLE_IDENTIFIER "org.existens.opensectional")
# Short application name — the user-directory segment on Linux/Windows.
set(APP_SHORT_NAME "osect")

# Inject the four OSECT_* string defines that every binary built from
# src/ needs (bundle id and app-name for per-user paths; display name
# and version for the startup banner and HTTP User-Agent). Applied to
# the main target and to each test target that compiles a src/ TU
# referencing one of these macros.
function(osect_apply_app_definitions target)
    target_compile_definitions(${target} PRIVATE
        OSECT_BUNDLE_IDENTIFIER="${BUNDLE_IDENTIFIER}"
        OSECT_APP_NAME="${APP_SHORT_NAME}"
        OSECT_APP_DISPLAY_NAME="${APPLICATION_NAME}"
        OSECT_APP_VERSION="${APPLICATION_VERSION}")
endfunction()

# Export compile commands
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Build-type-specific compiler options (MinGW lacks ASan support).
# RelWithDebInfo keeps frame pointers so sampling profilers can walk
# stacks without DWARF unwinding — free on AArch64 where the ABI
# already requires it, ~1% on x86_64.
if(MINGW)
    set(DEBUG_COMPILE_OPTIONS -O0 -g)
    set(RELWITHDEBINFO_COMPILE_OPTIONS -O2 -g -fno-omit-frame-pointer)
else()
    set(DEBUG_COMPILE_OPTIONS -O0 -ggdb -fsanitize=address)
    set(RELWITHDEBINFO_COMPILE_OPTIONS -O2 -g -fno-omit-frame-pointer)
endif()

find_package(Threads REQUIRED)

# OSECT_VENDOR_DEPS controls dependency sources only. OSECT_ENABLE_PACKAGING
# controls installer configuration; package builds require vendored
# dependencies so their output is self-contained. Both default OFF for the
# contributor build. Kept separate deliberately: VENDOR_DEPS=ON with
# PACKAGING=OFF builds the static (and on macOS, universal) release binary
# without the installer, so dependency bumps and release link problems can be
# worked on without first building osect.db, basemap/, and terrain/ — the
# packaging block aborts configure until all three exist.
option(OSECT_VENDOR_DEPS "Fetch pinned dependency sources and link them statically instead of using system packages" OFF)
option(OSECT_ENABLE_PACKAGING "Configure platform installer packaging" OFF)

if(OSECT_ENABLE_PACKAGING AND NOT OSECT_VENDOR_DEPS)
    message(FATAL_ERROR
        "OSECT_ENABLE_PACKAGING requires OSECT_VENDOR_DEPS=ON. "
        "Use the macos-package, mingw-package, or linux-package preset.")
endif()

if(OSECT_VENDOR_DEPS)
    # Vendored mode is supported on macOS, MinGW, and Linux.
    # Fetch the pinned dependency sources into <build>/_deps/.
    include(cmake/vendored_deps.cmake)

    # cmake/FindZLIB.cmake short-circuits curl's `find_package(ZLIB)` to
    # the in-tree zlibstatic target so the arch matches the rest of the
    # build (universal slicing on macOS, cross-compile on MinGW).
    list(PREPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

    # SDL3 — static-only, Vulkan on. SDL_VULKAN=ON is the default but
    # spelled out for explicitness across platforms.
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON  CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(SDL_VULKAN ON CACHE BOOL "" FORCE)
    add_subdirectory(${sdl3_SOURCE_DIR} ${sdl3_BINARY_DIR} EXCLUDE_FROM_ALL)

    # zlib — built in-tree as `zlibstatic`. Added before SDL_image and
    # curl so any vendored library that calls `find_package(ZLIB)`
    # (SDL_image's libtiff, libcurl) resolves through our FindZLIB shim.
    # ZLIB_BUILD_EXAMPLES OFF skips the example binaries;
    # ZLIB_BUILD_TESTING OFF skips registering zlib's example-as-tests
    # with our top-level ctest.
    set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(ZLIB_BUILD_TESTING  OFF CACHE BOOL "" FORCE)
    add_subdirectory(${zlib_SOURCE_DIR} ${zlib_BINARY_DIR} EXCLUDE_FROM_ALL)
    if(NOT TARGET ZLIB::ZLIB)
        add_library(ZLIB::ZLIB ALIAS zlibstatic)
    endif()

    # SDL3_image / SDL3_ttf — static, vendored decoders / vendored
    # freetype+harfbuzz. AVIF/WebP off to keep the static .a small (we
    # only load PNG tiles). SDLIMAGE_PNG_LIBPNG OFF routes PNG decoding
    # through SDL_image's built-in stb_image rather than its vendored
    # libpng — that skips SDL_image's separate vendored zlib (which
    # would conflict with our top-level vendored zlib).
    set(BUILD_SHARED_LIBS    OFF CACHE BOOL "" FORCE)
    set(SDLIMAGE_VENDORED    ON  CACHE BOOL "" FORCE)
    set(SDLIMAGE_AVIF        OFF CACHE BOOL "" FORCE)
    set(SDLIMAGE_WEBP        OFF CACHE BOOL "" FORCE)
    set(SDLIMAGE_PNG_LIBPNG  OFF CACHE BOOL "" FORCE)
    set(SDLIMAGE_TESTS       OFF CACHE BOOL "" FORCE)
    set(SDLIMAGE_SAMPLES     OFF CACHE BOOL "" FORCE)

    # SDL_image turns off its vendored libtiff's zlib support, and libtiff
    # warns when it then finds a libdeflate it can't use (e.g. the build
    # host's). Don't let it look for one.
    set(CMAKE_DISABLE_FIND_PACKAGE_Deflate TRUE)
    add_subdirectory(${sdl3_image_SOURCE_DIR} ${sdl3_image_BINARY_DIR} EXCLUDE_FROM_ALL)

    set(SDLTTF_VENDORED ON CACHE BOOL "" FORCE)
    set(SDLTTF_SAMPLES  OFF CACHE BOOL "" FORCE)
    add_subdirectory(${sdl3_ttf_SOURCE_DIR} ${sdl3_ttf_BINARY_DIR} EXCLUDE_FROM_ALL)

    # libcurl — Schannel on Windows, SecureTransport on macOS. Other
    # protocol/feature surface beyond HTTPS is disabled to keep the
    # static archive small and the configure surface narrow.
    set(BUILD_CURL_EXE      OFF CACHE BOOL "" FORCE)
    set(BUILD_STATIC_LIBS   ON  CACHE BOOL "" FORCE)
    set(BUILD_LIBCURL_DOCS  OFF CACHE BOOL "" FORCE)
    # Curl reads the global BUILD_TESTING; if we forced it OFF in
    # cache, our own option() call below would see the cached value
    # and skip osect's test suite. Save the caller's setting now and
    # restore it after curl's add_subdirectory, so curl's tests stay
    # off but ours stay under user control.
    set(_osect_saved_build_testing "${BUILD_TESTING}")
    set(BUILD_TESTING       OFF CACHE BOOL "" FORCE)
    # We never install or export-target curl from osect's build — it
    # links statically into the osect binary. Disabling install/export
    # avoids the "target X requires Y that is not in any export set"
    # diagnostic when curl's export references zlibstatic.
    set(CURL_DISABLE_INSTALL      ON  CACHE BOOL "" FORCE)
    set(CURL_ENABLE_EXPORT_TARGET OFF CACHE BOOL "" FORCE)
    set(CURL_USE_LIBPSL     OFF CACHE BOOL "" FORCE)
    set(CURL_USE_LIBSSH2    OFF CACHE BOOL "" FORCE)
    set(CURL_USE_LIBSSH     OFF CACHE BOOL "" FORCE)
    set(USE_LIBIDN2         OFF CACHE BOOL "" FORCE)
    set(CURL_USE_GSSAPI     OFF CACHE BOOL "" FORCE)
    set(USE_LIBRTMP         OFF CACHE BOOL "" FORCE)
    set(USE_NGHTTP2         OFF CACHE BOOL "" FORCE)
    set(CURL_BROTLI         OFF CACHE BOOL "" FORCE)
    set(CURL_ZSTD           OFF CACHE BOOL "" FORCE)
    set(CURL_ZLIB           ON  CACHE STRING "" FORCE)
    set(CURL_DISABLE_LDAP   ON  CACHE BOOL "" FORCE)
    set(CURL_DISABLE_LDAPS  ON  CACHE BOOL "" FORCE)
    set(CURL_DISABLE_RTSP   ON  CACHE BOOL "" FORCE)
    set(CURL_DISABLE_DICT   ON  CACHE BOOL "" FORCE)
    set(CURL_DISABLE_TELNET ON  CACHE BOOL "" FORCE)
    set(CURL_DISABLE_TFTP   ON  CACHE BOOL "" FORCE)
    set(CURL_DISABLE_POP3   ON  CACHE BOOL "" FORCE)
    set(CURL_DISABLE_IMAP   ON  CACHE BOOL "" FORCE)
    set(CURL_DISABLE_SMTP   ON  CACHE BOOL "" FORCE)
    set(CURL_DISABLE_SMB    ON  CACHE BOOL "" FORCE)
    set(CURL_DISABLE_GOPHER ON  CACHE BOOL "" FORCE)
    set(CURL_DISABLE_MQTT   ON  CACHE BOOL "" FORCE)
    set(ENABLE_MANUAL       OFF CACHE BOOL "" FORCE)
    set(PICKY_COMPILER      OFF CACHE BOOL "" FORCE)
    if(APPLE)
        set(CURL_USE_SECTRANSP ON  CACHE BOOL "" FORCE)
        set(CURL_USE_OPENSSL   OFF CACHE BOOL "" FORCE)
    elseif(WIN32)
        set(CURL_USE_SCHANNEL  ON  CACHE BOOL "" FORCE)
        set(CURL_USE_OPENSSL   OFF CACHE BOOL "" FORCE)
    else()
        # Linux has no OS TLS library with a stable ABI across distros, so
        # TLS is vendored mbedTLS, linked statically. (curl's OpenSSL
        # detection also can't work here: its try_compile checks link
        # ZLIB::ZLIB, an alias of our non-imported zlibstatic.)
        set(ENABLE_PROGRAMS        OFF CACHE BOOL "" FORCE)
        set(ENABLE_TESTING         OFF CACHE BOOL "" FORCE)
        set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
        add_subdirectory(${mbedtls_SOURCE_DIR} ${mbedtls_BINARY_DIR} EXCLUDE_FROM_ALL)
        set(CURL_USE_MBEDTLS   ON  CACHE BOOL "" FORCE)
        set(CURL_USE_OPENSSL   OFF CACHE BOOL "" FORCE)
        # Pre-set what curl's FindMbedTLS would search for, naming the
        # vendored targets, so it links them instead of system libraries.
        set(MBEDTLS_INCLUDE_DIR "${mbedtls_SOURCE_DIR}/include" CACHE PATH "" FORCE)
        set(MBEDTLS_LIBRARY     mbedtls    CACHE STRING "" FORCE)
        set(MBEDX509_LIBRARY    mbedx509   CACHE STRING "" FORCE)
        set(MBEDCRYPTO_LIBRARY  mbedcrypto CACHE STRING "" FORCE)
        # No build-time CA location: the trust store's path differs per
        # distro, so http_client finds it at runtime instead.
        set(CURL_CA_BUNDLE      none CACHE STRING "" FORCE)
        set(CURL_CA_PATH        none CACHE STRING "" FORCE)
    endif()
    add_subdirectory(${curl_SOURCE_DIR} ${curl_BINARY_DIR} EXCLUDE_FROM_ALL)
    # Restore BUILD_TESTING (see save above) so the osect tests block
    # sees the caller's intent.
    if(DEFINED _osect_saved_build_testing AND NOT "${_osect_saved_build_testing}" STREQUAL "")
        set(BUILD_TESTING "${_osect_saved_build_testing}" CACHE BOOL "" FORCE)
    else()
        unset(BUILD_TESTING CACHE)
    endif()

    # SQLite amalgamation — single C file built into a static library.
    # RTREE = spatial bbox queries; FTS5 = the search index built by
    # tools/build_search.py. Both off by default in the amalgamation;
    # we need them.
    add_library(sqlite3 STATIC ${sqlite_SOURCE_DIR}/sqlite3.c)
    target_include_directories(sqlite3 PUBLIC ${sqlite_SOURCE_DIR})
    target_compile_definitions(sqlite3 PRIVATE
        SQLITE_ENABLE_RTREE=1
        SQLITE_ENABLE_FTS5=1)
    target_compile_options(sqlite3 PRIVATE -O2)
    set(SQLITE3_TARGET sqlite3)

    # Warnings newer GCC (16) raises in vendored code, silenced where they
    # occur. glibc 2.43's C23 string functions return const-qualified
    # pointers for const inputs, which curl and SQLite assign to plain
    # pointers.
    target_compile_options(libcurl_static PRIVATE $<$<C_COMPILER_ID:GNU>:-Wno-discarded-qualifiers>)
    target_compile_options(sqlite3 PRIVATE $<$<C_COMPILER_ID:GNU>:-Wno-discarded-qualifiers>)

    # glibc 2.43 also defines strstr as a macro, which SDL_image's SVG
    # loader redefines without #undef. GCC has no flag for plain macro
    # redefinition, so this one file gets -w.
    set_source_files_properties("${sdl3_image_SOURCE_DIR}/src/IMG_svg.c"
        DIRECTORY "${sdl3_image_SOURCE_DIR}"
        PROPERTIES COMPILE_OPTIONS $<$<C_COMPILER_ID:GNU>:-w>)
else()
    # System mode (default for contributor builds). Each dependency
    # comes from the host's package manager — apt, brew, pacman/MSYS2.
    # Runtime links are dynamic, and cmake/vendored_deps.cmake fetches
    # nothing. SDL3 3.2 is the first stable line shipped in distros. The
    # vendored SDL is pinned ahead of this for release builds,
    # but contributors using system packages on Ubuntu/brew/MSYS2 get
    # whatever stable point release the distro ships.
    find_package(SDL3       3.2 REQUIRED CONFIG)
    find_package(SDL3_image     REQUIRED CONFIG)
    find_package(SDL3_ttf       REQUIRED CONFIG)
    find_package(CURL           REQUIRED)
    find_package(ZLIB           REQUIRED)
    find_package(PkgConfig      REQUIRED)
    pkg_check_modules(SQLITE3 REQUIRED IMPORTED_TARGET sqlite3)
    set(SQLITE3_TARGET PkgConfig::SQLITE3)
endif()

# Third-party libraries (header-only / single-source vendored)
add_subdirectory(thirdparty/imgui)

# pugixml — vendored XML parser used by the XNOTAM (TFR / NOTAM) path.
# Single source file; build it as its own static library so we can
# expose the include dir to the app without leaking it elsewhere.
add_library(pugixml STATIC thirdparty/pugixml-1.14/pugixml.cpp)
target_include_directories(pugixml PUBLIC thirdparty/pugixml-1.14)
target_compile_features(pugixml PRIVATE cxx_std_11)
target_compile_options(pugixml PRIVATE
    $<$<CONFIG:Debug>:${DEBUG_COMPILE_OPTIONS}>
    $<$<CONFIG:Release>:-O3>
    $<$<CONFIG:RelWithDebInfo>:${RELWITHDEBINFO_COMPILE_OPTIONS}>
)

# Tests (doctest, opt-in). Built as a separate binary with ctest integration.
option(BUILD_TESTING "Build unit tests" ON)
if(BUILD_TESTING)
    enable_testing()
    add_subdirectory(tests)
endif()

# D3D12 backend: experimental Windows-only path. Requires dxc to compile DXIL.
# Auto-disabled if dxc is not available, even when ON. Vulkan is always built.
option(OSECT_ENABLE_D3D12 "Build with experimental D3D12 backend (Windows only, requires dxc)" ON)

# Precompiled .metallib pipeline (macOS only). Default ON; auto-disabled if the
# metal compiler isn't available. Set OFF explicitly to test the MSL-source
# fallback path even when full Xcode is installed.
option(OSECT_ENABLE_METALLIB "Embed precompiled .metallib bytecode on macOS (requires full Xcode)" ON)

# Main executable
add_executable(osect
    # Application sources
    src/app_options.cpp
    src/ini_config.cpp
    src/geo_math.cpp
    src/map_view.cpp
    src/map_widget.cpp
    src/flight_route.cpp
    src/feature_builder.cpp
    src/feature_type.cpp
    src/feature_renderer.cpp
    src/label_renderer.cpp
    src/line_renderer.cpp
    src/main.cpp
    src/program.cpp
    src/nasr_database.cpp
    src/ui_popup_manager.cpp
    src/route_plan_config.cpp
    src/route_planner.cpp
    src/route_session.cpp
    src/route_submitter.cpp
    src/tile_loader.cpp
    src/chart_style.cpp
    src/chart_type.cpp
    src/data_source.cpp
    src/elevation_source.cpp
    src/elevation_tile.cpp
    src/terrain_profile.cpp
    src/terrain_profile_worker.cpp
    src/terrain_renderer.cpp
    src/terrain_style.cpp
    src/ephemeral_database.cpp
    src/ephemeral_source.cpp
    src/http_client.cpp
    src/tfr_refresher.cpp
    src/tile_renderer.cpp
    src/ui_overlay.cpp
    src/ui_profile_panel.cpp
    src/ui_sectioned_list.cpp
    src/user_database.cpp
    src/waypoint_session.cpp
    src/xnotam_parser.cpp
)
osect_apply_app_definitions(osect)

# SDL C++ wrapper library
add_library(sdl_wrapper STATIC
    lib/sdl/buffer.cpp
    lib/sdl/command_buffer.cpp
    lib/sdl/copy_pass.cpp
    lib/sdl/depth_buffer.cpp
    lib/sdl/device.cpp
    lib/sdl/error.cpp
    lib/sdl/event.cpp
    lib/sdl/filesystem.cpp
    lib/sdl/font.cpp
    lib/sdl/instance.cpp
    lib/sdl/log.cpp
    lib/sdl/pipeline.cpp
    lib/sdl/render_pass.cpp
    lib/sdl/sampler.cpp
    lib/sdl/shader.cpp
    lib/sdl/surface.cpp
    lib/sdl/text.cpp
    lib/sdl/text_engine.cpp
    lib/sdl/texture.cpp
    lib/sdl/timer.cpp
    lib/sdl/transfer_buffer.cpp
    lib/sdl/types.cpp
    lib/sdl/window.cpp
)
target_compile_features(sdl_wrapper PRIVATE cxx_std_11)
target_include_directories(sdl_wrapper PUBLIC
    ${CMAKE_SOURCE_DIR}/lib
    ${CMAKE_SOURCE_DIR}/thirdparty/glm-1.0.1
    ${CMAKE_SOURCE_DIR}/thirdparty
)
target_link_libraries(sdl_wrapper PUBLIC SDL3::SDL3 SDL3_image::SDL3_image SDL3_ttf::SDL3_ttf)
target_compile_options(sdl_wrapper PRIVATE -Wall -Wextra)
target_compile_options(sdl_wrapper PRIVATE
    $<$<CONFIG:Debug>:${DEBUG_COMPILE_OPTIONS}>
    $<$<CONFIG:Release>:-O3>
    $<$<CONFIG:RelWithDebInfo>:${RELWITHDEBINFO_COMPILE_OPTIONS}>
)

# ImGui C++ wrapper library
add_library(imgui_wrapper STATIC
    lib/imgui/context.cpp
    lib/imgui/scoped.cpp
)
target_compile_features(imgui_wrapper PRIVATE cxx_std_11)
target_include_directories(imgui_wrapper PRIVATE ${CMAKE_SOURCE_DIR}/lib)
target_link_libraries(imgui_wrapper PUBLIC sdl_wrapper imgui)
target_compile_options(imgui_wrapper PRIVATE -Wall -Wextra)
target_compile_options(imgui_wrapper PRIVATE
    $<$<CONFIG:Debug>:${DEBUG_COMPILE_OPTIONS}>
    $<$<CONFIG:Release>:-O3>
    $<$<CONFIG:RelWithDebInfo>:${RELWITHDEBINFO_COMPILE_OPTIONS}>
)

# SQLite C++ wrapper library
add_library(sqlite_wrapper STATIC
    lib/sqlite/database.cpp
    lib/sqlite/error.cpp
    lib/sqlite/statement.cpp
)
target_compile_features(sqlite_wrapper PRIVATE cxx_std_11)
target_include_directories(sqlite_wrapper PUBLIC ${CMAKE_SOURCE_DIR}/lib)
target_link_libraries(sqlite_wrapper PUBLIC ${SQLITE3_TARGET})
target_compile_options(sqlite_wrapper PRIVATE -Wall -Wextra)
target_compile_options(sqlite_wrapper PRIVATE
    $<$<CONFIG:Debug>:${DEBUG_COMPILE_OPTIONS}>
    $<$<CONFIG:Release>:-O3>
    $<$<CONFIG:RelWithDebInfo>:${RELWITHDEBINFO_COMPILE_OPTIONS}>
)

# Include directories
target_include_directories(osect PRIVATE
    ${CMAKE_SOURCE_DIR}/src    # For #include "layer.hpp" etc.
    ${CMAKE_SOURCE_DIR}/thirdparty
)

# C++ standard
target_compile_features(osect PRIVATE cxx_std_17)

# Compiler warnings (all build types)
target_compile_options(osect PRIVATE -Wall -Wextra -Werror)

# Build-type-specific compiler options
target_compile_options(osect PRIVATE
    $<$<CONFIG:Debug>:${DEBUG_COMPILE_OPTIONS}>
    $<$<CONFIG:Release>:-O3>
    $<$<CONFIG:RelWithDebInfo>:${RELWITHDEBINFO_COMPILE_OPTIONS}>
)

# Build-type-specific definitions
target_compile_definitions(osect PRIVATE
    $<$<CONFIG:Debug>:DEBUG=1>
)

# Build-type-specific linker options
target_link_options(osect PRIVATE
    $<$<CONFIG:Debug>:${DEBUG_COMPILE_OPTIONS}>
    $<$<CONFIG:RelWithDebInfo>:${RELWITHDEBINFO_COMPILE_OPTIONS}>
)

# On Windows (MinGW), Release builds use the GUI subsystem so launching the
# app doesn't spawn a console window. Debug/RelWithDebInfo keep the default
# console subsystem so stdout/stderr remain visible for diagnostics.
if(MINGW)
    target_link_options(osect PRIVATE $<$<CONFIG:Release>:-mwindows>)
endif()

# Vendored Linux builds are meant to run on other distros, so link the GCC
# runtime statically and depend only on glibc (SDL loads X11 / Wayland /
# audio / Vulkan libraries from the running system).
if(OSECT_VENDOR_DEPS AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_options(osect PRIVATE -static-libstdc++ -static-libgcc)
endif()

# Link libraries. CURL::libcurl is the in-tree static libcurl whose
# transitive deps (SecureTransport frameworks on macOS, Schannel /
# winsock on Windows, zlibstatic everywhere) ride along through the
# imported target's INTERFACE_LINK_LIBRARIES.
target_link_libraries(osect PRIVATE
    imgui_wrapper
    sqlite_wrapper
    pugixml
    Threads::Threads
    CURL::libcurl
)

# Embed font as C header
find_program(XXD xxd REQUIRED)
set(FONT_SOURCE "${CMAKE_SOURCE_DIR}/thirdparty/fonts/NotoSans-Regular.ttf")
set(FONT_HEADER "${CMAKE_BINARY_DIR}/NotoSans_Regular_ttf.h")
add_custom_command(
    OUTPUT ${FONT_HEADER}
    COMMAND ${XXD} -i NotoSans-Regular.ttf > ${FONT_HEADER}
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/thirdparty/fonts"
    DEPENDS ${FONT_SOURCE}
    COMMENT "Embedding font: NotoSans-Regular.ttf"
    VERBATIM
)
add_custom_target(embedded_font DEPENDS ${FONT_HEADER})

# Shader compilation (Metal, SPIRV, optional DXIL)
add_subdirectory(shaders)

# Add shader and font headers to include path
target_include_directories(osect PRIVATE ${SHADER_INCLUDE_DIR} ${CMAKE_BINARY_DIR})

# Ensure shaders and font are built before the executable
add_dependencies(osect shaders embedded_font)

# Propagate D3D12 build state to source code (gates DXIL bytecode includes,
# the SDL_GPU shader-formats mask, and the --gpu direct3d12 runtime check).
if(OSECT_BUILD_D3D12)
    target_compile_definitions(osect PRIVATE OSECT_HAVE_DXIL=1)
    target_compile_definitions(sdl_wrapper PRIVATE SDL_WRAPPER_HAVE_DXIL=1)
endif()

# Propagate Metal-library build state. When set, the build embeds precompiled
# .metallib bytecode (fastest startup); when not, the macOS runtime falls back
# to compiling the embedded MSL source via the Metal driver on first use.
if(OSECT_BUILD_METALLIB)
    target_compile_definitions(osect PRIVATE OSECT_HAVE_METALLIB=1)
    target_compile_definitions(sdl_wrapper PRIVATE SDL_WRAPPER_HAVE_METALLIB=1)
endif()

# =============================================================================
# clang-tidy target
# =============================================================================
find_program(RUN_CLANG_TIDY run-clang-tidy)
find_program(CLANG_TIDY clang-tidy)
include(ProcessorCount)
ProcessorCount(NPROC)
if(NPROC EQUAL 0)
    set(NPROC 4)
endif()
if(RUN_CLANG_TIDY)
    add_custom_target(clang-tidy
        COMMAND ${RUN_CLANG_TIDY}
            -p ${CMAKE_BINARY_DIR}
            -j ${NPROC}
            -source-filter "^${CMAKE_SOURCE_DIR}/(src|lib)/.*\\.cpp$$"
            -quiet
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Running clang-tidy on src/ and lib/ (${NPROC} jobs)"
        VERBATIM
    )
elseif(CLANG_TIDY)
    file(GLOB _TIDY_SOURCES
        ${CMAKE_SOURCE_DIR}/src/*.cpp
        ${CMAKE_SOURCE_DIR}/lib/sdl/*.cpp
        ${CMAKE_SOURCE_DIR}/lib/sqlite/*.cpp
        ${CMAKE_SOURCE_DIR}/lib/imgui/*.cpp)
    add_custom_target(clang-tidy
        COMMAND ${CLANG_TIDY} -p ${CMAKE_BINARY_DIR} ${_TIDY_SOURCES}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Running clang-tidy on src/ and lib/ (sequential)"
        VERBATIM
    )
endif()

# =============================================================================
# clang-format targets
# =============================================================================
# `clang-format`       — rewrite src/ and lib/ in place using .clang-format.
# `clang-format-check` — dry-run; non-zero exit if any file would change.
find_program(CLANG_FORMAT clang-format)
if(CLANG_FORMAT)
    file(GLOB _FORMAT_SOURCES
        ${CMAKE_SOURCE_DIR}/src/*.cpp
        ${CMAKE_SOURCE_DIR}/src/*.hpp
        ${CMAKE_SOURCE_DIR}/src/*.h
        ${CMAKE_SOURCE_DIR}/lib/imgui/*.cpp
        ${CMAKE_SOURCE_DIR}/lib/imgui/*.hpp
        ${CMAKE_SOURCE_DIR}/lib/sdl/*.cpp
        ${CMAKE_SOURCE_DIR}/lib/sdl/*.hpp
        ${CMAKE_SOURCE_DIR}/lib/sqlite/*.cpp
        ${CMAKE_SOURCE_DIR}/lib/sqlite/*.hpp)
    add_custom_target(clang-format
        COMMAND ${CLANG_FORMAT} -i ${_FORMAT_SOURCES}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Applying clang-format to src/ and lib/"
        VERBATIM
    )
    add_custom_target(clang-format-check
        COMMAND ${CLANG_FORMAT} --dry-run --Werror ${_FORMAT_SOURCES}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Checking clang-format conformance for src/ and lib/"
        VERBATIM
    )
endif()

# =============================================================================
# Installer / packaging
# =============================================================================
#
# Prebuilt assets that ship with the installer. They are not in source control;
# regenerate them via tools/ before running `cpack`.
set(BUNDLED_DB      "${CMAKE_SOURCE_DIR}/osect.db")
set(BUNDLED_BASEMAP "${CMAKE_SOURCE_DIR}/basemap")
set(BUNDLED_TERRAIN "${CMAKE_SOURCE_DIR}/terrain")

set(ICON_PNG            "${CMAKE_SOURCE_DIR}/osect.png")
set(INSTALLER_ICON_ICNS "${CMAKE_BINARY_DIR}/osect.icns")
set(INSTALLER_ICON_ICO  "${CMAKE_BINARY_DIR}/osect.ico")

# Platform-specific icon generation from the master PNG. Skipped silently
# if the required tool is missing — installer blocks below make the icon
# optional.
if(APPLE AND OSECT_ENABLE_PACKAGING AND EXISTS "${ICON_PNG}")
    # tools/build_macos_icon.sh takes the raw PNG all the way to a .icns
    # (shape → iconset slicing → icns packaging). CMake just locates the
    # tools the script depends on and invokes it.
    find_program(SIPS sips)
    find_program(ICONUTIL iconutil)
    find_program(MAGICK magick)
    if(SIPS AND ICONUTIL AND MAGICK)
        set(_build_macos_icon "${CMAKE_SOURCE_DIR}/tools/build_macos_icon.sh")
        add_custom_command(
            OUTPUT "${INSTALLER_ICON_ICNS}"
            COMMAND ${CMAKE_COMMAND} -E env "MAGICK=${MAGICK}"
                    "${_build_macos_icon}" "${ICON_PNG}" "${INSTALLER_ICON_ICNS}"
            DEPENDS "${ICON_PNG}" "${_build_macos_icon}"
            COMMENT "Generating ${INSTALLER_ICON_ICNS}"
            VERBATIM
        )
        add_custom_target(app_icon DEPENDS "${INSTALLER_ICON_ICNS}")
        add_dependencies(osect app_icon)
    endif()
endif()

if(WIN32 AND OSECT_ENABLE_PACKAGING AND EXISTS "${ICON_PNG}")
    # tools/build_windows_icon.sh takes the raw PNG all the way to a .ico
    # (circle mask → multi-size .ico packaging). CMake just locates the
    # tools the script depends on and invokes it.
    find_program(MAGICK magick)
    # Invoke the icon script via bash explicitly: on Windows the build
    # runner is cmd.exe (via Ninja/make), which has no #! shebang concept
    # and can't execute .sh files directly. MSYS2 always supplies bash.
    find_program(BASH bash)
    if(MAGICK AND BASH)
        set(_build_windows_icon "${CMAKE_SOURCE_DIR}/tools/build_windows_icon.sh")
        add_custom_command(
            OUTPUT "${INSTALLER_ICON_ICO}"
            COMMAND ${CMAKE_COMMAND} -E env "MAGICK=${MAGICK}"
                    "${BASH}" "${_build_windows_icon}" "${ICON_PNG}" "${INSTALLER_ICON_ICO}"
            DEPENDS "${ICON_PNG}" "${_build_windows_icon}"
            COMMENT "Generating ${INSTALLER_ICON_ICO}"
            VERBATIM
        )
        add_custom_target(app_icon DEPENDS "${INSTALLER_ICON_ICO}")
        add_dependencies(osect app_icon)

        # Embed the .ico as a Windows resource so osect.exe carries its own
        # icon. Without this, Explorer, the taskbar, and the "Installed Apps"
        # uninstall list (which reads DisplayIcon → osect.exe from the
        # registry, set via CPACK_NSIS_INSTALLED_ICON_NAME below) all fall
        # back to a generic icon.
        enable_language(RC)
        set(_osect_rc "${CMAKE_BINARY_DIR}/osect.rc")
        file(WRITE "${_osect_rc}" "IDI_ICON1 ICON \"${INSTALLER_ICON_ICO}\"\n")
        target_sources(osect PRIVATE "${_osect_rc}")
        set_source_files_properties("${_osect_rc}" PROPERTIES
            OBJECT_DEPENDS "${INSTALLER_ICON_ICO}")
    endif()
endif()

if(OSECT_ENABLE_PACKAGING)
    foreach(_asset "${BUNDLED_DB}" "${BUNDLED_BASEMAP}" "${BUNDLED_TERRAIN}")
        if(NOT EXISTS "${_asset}")
            message(FATAL_ERROR
                "Installer asset missing: ${_asset}\n"
                "Generate prebuilt assets (see README 'Data Preparation') before packaging.")
        endif()
    endforeach()

    # Settings every installer shares; each platform branch below adds its
    # install layout and generator settings.
    set(CPACK_PACKAGE_NAME "${APPLICATION_NAME}")
    set(CPACK_PACKAGE_VENDOR "existens.org")
    set(CPACK_PACKAGE_VERSION_MAJOR "${MAJOR_VERSION}")
    set(CPACK_PACKAGE_VERSION_MINOR "${MINOR_VERSION}")
    set(CPACK_PACKAGE_VERSION_PATCH "${BUILD_VERSION}")
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Open source chart viewer and flight planner")
    if(EXISTS "${CMAKE_SOURCE_DIR}/LICENSE")
        set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/LICENSE")
    endif()

    if(APPLE)
        # =========================================================================
        # macOS: bundle + DragNDrop DMG
        # =========================================================================
        set_target_properties(osect PROPERTIES
            OUTPUT_NAME "${APPLICATION_NAME}"
            MACOSX_BUNDLE TRUE
            MACOSX_BUNDLE_BUNDLE_NAME "${APPLICATION_NAME}"
            MACOSX_BUNDLE_BUNDLE_VERSION "${APPLICATION_VERSION}"
            MACOSX_BUNDLE_SHORT_VERSION_STRING "${APPLICATION_VERSION}"
            MACOSX_BUNDLE_GUI_IDENTIFIER "${BUNDLE_IDENTIFIER}"
            MACOSX_BUNDLE_COPYRIGHT "existens.org"
        )
        if(TARGET app_icon)
            set_target_properties(osect PROPERTIES
                MACOSX_BUNDLE_ICON_FILE "osect.icns")
            target_sources(osect PRIVATE "${INSTALLER_ICON_ICNS}")
            set_source_files_properties("${INSTALLER_ICON_ICNS}" PROPERTIES
                MACOSX_PACKAGE_LOCATION Resources
                GENERATED TRUE)
        endif()

        install(TARGETS osect BUNDLE DESTINATION .)

        # Bundled assets go into Contents/Resources/ alongside the icon
        set(_rsrc "${APPLICATION_NAME}.app/Contents/Resources")
        install(FILES "${BUNDLED_DB}"  DESTINATION "${_rsrc}")
        install(DIRECTORY "${BUNDLED_BASEMAP}" DESTINATION "${_rsrc}")
        install(DIRECTORY "${BUNDLED_TERRAIN}" DESTINATION "${_rsrc}")

        # Bundle MoltenVK so the .app's Vulkan backend works on a fresh Mac
        # with no Vulkan SDK / Homebrew installed. SDL3 searches
        # @executable_path/../Frameworks/libMoltenVK.dylib first when loading
        # Vulkan on macOS, so this just works once the file is in place. The
        # precompiled universal dylib is fetched by cmake/vendored_deps.cmake;
        # the paths below match the pinned release's archive layout.
        install(FILES "${moltenvk_SOURCE_DIR}/MoltenVK/dynamic/dylib/macOS/libMoltenVK.dylib"
                DESTINATION "${APPLICATION_NAME}.app/Contents/Frameworks")
        # Apache-2.0 requires shipping the LICENSE alongside the binary.
        install(FILES "${moltenvk_SOURCE_DIR}/LICENSE"
                DESTINATION "${_rsrc}"
                RENAME "LICENSE-MoltenVK.txt")

        # Ad-hoc sign the installed bundle as a whole (executable, bundled
        # MoltenVK, Info.plist and resources) so it launches on Apple Silicon.
        # Runs inside the install prefix, after every file is in place.
        install(CODE "
            set(_bundle \"\${CMAKE_INSTALL_PREFIX}/${APPLICATION_NAME}.app\")
            execute_process(
                COMMAND codesign --force --deep --sign - \"\${_bundle}\"
                RESULT_VARIABLE _codesign_rc)
            if(NOT _codesign_rc EQUAL 0)
                message(FATAL_ERROR \"codesign --sign - failed (rc=\${_codesign_rc})\")
            endif()
        " COMPONENT applications)

        set(CPACK_GENERATOR "DragNDrop")
        set(CPACK_DMG_VOLUME_NAME "${APPLICATION_NAME} ${APPLICATION_VERSION}")
        set(CPACK_DMG_FORMAT "UDZO")
        # Avoids `hdiutil create -srcfolder`, which needs root authorization
        # when the build tree's files belong to another uid (network shares);
        # see the script header.
        set(CPACK_COMMAND_HDIUTIL "${CMAKE_SOURCE_DIR}/cmake/hdiutil_wrapper.sh")

    elseif(WIN32)
        # =========================================================================
        # Windows: flat install tree + NSIS installer
        # =========================================================================
        # Everything lands directly in $INSTDIR (e.g. C:\Program Files\OpenSectional).
        # This is the conventional Windows layout for a single-binary app: the
        # exe sits next to its DLLs (Windows resolves them from the exe's dir)
        # and next to its data assets (osect's runtime asset resolution looks
        # alongside the executable). No bin/ subdir, which is a Unix-ism.
        install(TARGETS osect RUNTIME DESTINATION .)
        install(FILES "${BUNDLED_DB}"  DESTINATION .)
        install(DIRECTORY "${BUNDLED_BASEMAP}" DESTINATION .)
        install(DIRECTORY "${BUNDLED_TERRAIN}" DESTINATION .)

        # MinGW runtime DLLs (SDL3/SDL3_image/SDL3_ttf/sqlite3 are statically
        # linked when OSECT_VENDOR_DEPS=ON, so only
        # libstdc++/libgcc/winpthread need to ship).
        set(_mingw_runtime_dlls
            libgcc_s_seh-1.dll
            libstdc++-6.dll
            libwinpthread-1.dll
        )
        foreach(_dll ${_mingw_runtime_dlls})
            execute_process(
                COMMAND ${CMAKE_CXX_COMPILER} -print-file-name=${_dll}
                OUTPUT_VARIABLE _dll_path
                OUTPUT_STRIP_TRAILING_WHITESPACE
            )
            if(_dll_path AND NOT _dll_path STREQUAL "${_dll}" AND EXISTS "${_dll_path}")
                install(FILES "${_dll_path}" DESTINATION .)
            endif()
        endforeach()

        set(CPACK_GENERATOR "NSIS")
        set(CPACK_PACKAGE_INSTALL_DIRECTORY "${APPLICATION_NAME}")

        set(CPACK_NSIS_DISPLAY_NAME "${APPLICATION_NAME}")
        set(CPACK_NSIS_PACKAGE_NAME "${APPLICATION_NAME}")
        set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
        set(CPACK_NSIS_MODIFY_PATH OFF)
        set(CPACK_NSIS_INSTALLED_ICON_NAME "osect.exe")
        if(TARGET app_icon)
            set(CPACK_NSIS_MUI_ICON    "${INSTALLER_ICON_ICO}")
            set(CPACK_NSIS_MUI_UNIICON "${INSTALLER_ICON_ICO}")
        endif()

        # Start menu shortcut
        set(CPACK_NSIS_CREATE_ICONS_EXTRA
            "CreateShortCut '$SMPROGRAMS\\\\$STARTMENU_FOLDER\\\\${APPLICATION_NAME}.lnk' '$INSTDIR\\\\osect.exe'")
        set(CPACK_NSIS_DELETE_ICONS_EXTRA
            "Delete '$SMPROGRAMS\\\\$STARTMENU_FOLDER\\\\${APPLICATION_NAME}.lnk'")

    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        # =========================================================================
        # Linux: AppImage + .tar.xz of the same tree
        # =========================================================================
        if(CMAKE_VERSION VERSION_LESS 4.2)
            message(FATAL_ERROR "Linux packaging needs CMake 4.2 or newer (CPack AppImage generator); this is ${CMAKE_VERSION}.")
        endif()
        # bin/osect with its data under share/osect/, which the runtime asset
        # lookup finds relative to the executable. The tree is the AppDir for
        # the AppImage and the contents of the tarball.
        install(TARGETS osect RUNTIME DESTINATION bin)
        install(FILES "${BUNDLED_DB}"  DESTINATION share/${APP_SHORT_NAME})
        install(DIRECTORY "${BUNDLED_BASEMAP}" DESTINATION share/${APP_SHORT_NAME})
        install(DIRECTORY "${BUNDLED_TERRAIN}" DESTINATION share/${APP_SHORT_NAME})

        # The AppImage generator requires a desktop entry whose Exec and Icon
        # name installed files; it links both to the AppDir's top level.
        set(_desktop_file "${CMAKE_BINARY_DIR}/${APP_SHORT_NAME}.desktop")
        file(WRITE "${_desktop_file}"
            "[Desktop Entry]\n"
            "Type=Application\n"
            "Name=${APPLICATION_NAME}\n"
            "Comment=Aeronautical chart viewer and flight planner\n"
            "Exec=${APP_SHORT_NAME}\n"
            "Icon=${APP_SHORT_NAME}\n"
            "Categories=Science;Geography;Maps;\n"
            "Terminal=false\n")
        install(FILES "${_desktop_file}" DESTINATION share/applications)
        install(FILES "${ICON_PNG}" DESTINATION share/icons/hicolor/1024x1024/apps
                RENAME "${APP_SHORT_NAME}.png")

        include(cmake/appimage_tools.cmake)

        set(CPACK_GENERATOR "AppImage;TXZ")
        set(CPACK_PACKAGE_FILE_NAME "${APPLICATION_NAME}-${APPLICATION_VERSION}-${CMAKE_SYSTEM_PROCESSOR}")
        set(CPACK_PACKAGE_ICON "${APP_SHORT_NAME}.png")
        set(CPACK_APPIMAGE_NO_APPSTREAM ON)

    else()
        message(FATAL_ERROR "OSECT_ENABLE_PACKAGING: no installer is defined for ${CMAKE_SYSTEM_NAME}.")
    endif()

    include(CPack)
endif()
