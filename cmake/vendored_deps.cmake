# Pinned sources for OSECT_VENDOR_DEPS=ON builds, fetched at configure time
# into the build directory (<build>/_deps/). Nothing is written to the source
# tree. Included from the top-level CMakeLists.txt; after it returns, each
# dependency's <name>_SOURCE_DIR / <name>_BINARY_DIR are set and the caller
# add_subdirectory()s the CMake projects with its own cache settings.
#
# Every dependency is a SHA-256-pinned archive; the build never runs git.
# Top-level projects use their official release archives. Those omit the
# nested submodules SDL_image and SDL_ttf vendor under external/, so each
# nested library the build uses is fetched separately as a GitHub archive of
# the exact commit the parent release pins, and unpacked into the parent's
# external/ directory. Unused nested submodules (SDL_image's aom, dav1d, ...;
# freetype's dlg; plutosvg's own plutovg copy) are not fetched.
#
# Bumping a parent: look up the commits its new release tag pins for each
# external/ submodule (`git ls-tree <tag> external/`), update the nested
# URLs and hashes below, and configure a fresh build directory (re-extracting
# a parent replaces its external/ directory).

include(FetchContent)

# Silence the extracted-timestamp warning for URL downloads (CMake >= 3.24).
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()

# SOURCE_SUBDIR points at a directory that doesn't exist so
# FetchContent_MakeAvailable() only populates the sources; the top-level
# CMakeLists add_subdirectory()s each project itself, with EXCLUDE_FROM_ALL
# and the cache settings it needs set first.
set(_osect_populate_only SOURCE_SUBDIR _osect_populate_only)

FetchContent_Declare(sdl3
    URL      https://github.com/libsdl-org/SDL/releases/download/release-3.4.4/SDL3-3.4.4.tar.gz
    URL_HASH SHA256=ee712dbe6a89bb140bbfc2ce72358fb5ee5cc2240abeabd54855012db30b3864
    ${_osect_populate_only})

FetchContent_Declare(sdl3_image
    URL      https://github.com/libsdl-org/SDL_image/releases/download/release-3.4.2/SDL3_image-3.4.2.tar.gz
    URL_HASH SHA256=82fdb88cf1a9cbdc1c77797aaa3292e6d22ce12586be718c8ea43530df1536b4
    SOURCE_DIR ${FETCHCONTENT_BASE_DIR}/sdl3_image-src
    ${_osect_populate_only})

# external/libtiff: TIFF support is vendored on MinGW (macOS uses ImageIO).
FetchContent_Declare(sdl3_image_libtiff
    URL      https://github.com/libsdl-org/libtiff/archive/442c4cf610ed5271c0af79ecb5df2c09394c6736.tar.gz
    URL_HASH SHA256=e288cd797e511b8ba3d227ac9639b3e7d69614483d4995a77a849c3031e8efda
    SOURCE_DIR ${FETCHCONTENT_BASE_DIR}/sdl3_image-src/external/libtiff
    ${_osect_populate_only})

FetchContent_Declare(sdl3_ttf
    URL      https://github.com/libsdl-org/SDL_ttf/releases/download/release-3.2.2/SDL3_ttf-3.2.2.tar.gz
    URL_HASH SHA256=63547d58d0185c833213885b635a2c0548201cc8f301e6587c0be1a67e1e045d
    SOURCE_DIR ${FETCHCONTENT_BASE_DIR}/sdl3_ttf-src
    ${_osect_populate_only})

FetchContent_Declare(sdl3_ttf_freetype
    URL      https://github.com/libsdl-org/freetype/archive/9973564cfa63763a3e4ac67c09147899539b1e07.tar.gz
    URL_HASH SHA256=026a05a49d114a1235d2926f4c03a9330e4b1a6efe7c217ec9607904c32907d4
    SOURCE_DIR ${FETCHCONTENT_BASE_DIR}/sdl3_ttf-src/external/freetype
    ${_osect_populate_only})

FetchContent_Declare(sdl3_ttf_harfbuzz
    URL      https://github.com/libsdl-org/harfbuzz/archive/564bf9818a18709776856533829c0c04950773d6.tar.gz
    URL_HASH SHA256=a448dd6c22d8e1e1cf39438c662251c1f97f810b8780eed4a6d6ada948c99ddc
    SOURCE_DIR ${FETCHCONTENT_BASE_DIR}/sdl3_ttf-src/external/harfbuzz
    ${_osect_populate_only})

FetchContent_Declare(sdl3_ttf_plutosvg
    URL      https://github.com/libsdl-org/plutosvg/archive/2983eb6919feea272d793bc386384e3f5b97b03c.tar.gz
    URL_HASH SHA256=cd416a2bbc63f5ca9c7a3582ce0c06e69e15b6087aef991cf36c38f68851dc17
    SOURCE_DIR ${FETCHCONTENT_BASE_DIR}/sdl3_ttf-src/external/plutosvg
    ${_osect_populate_only})

FetchContent_Declare(sdl3_ttf_plutovg
    URL      https://github.com/libsdl-org/plutovg/archive/3e6f922f453da1c9e7d1d7f66cac1d9724a18b47.tar.gz
    URL_HASH SHA256=491ee12761688a61f98bf956fa858d4610d106ade950e0530f5b19ce195d0164
    SOURCE_DIR ${FETCHCONTENT_BASE_DIR}/sdl3_ttf-src/external/plutovg
    ${_osect_populate_only})

FetchContent_Declare(zlib
    URL      https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.gz
    URL_HASH SHA256=9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23
    ${_osect_populate_only})

FetchContent_Declare(curl
    URL      https://curl.se/download/curl-8.13.0.tar.xz
    URL_HASH SHA256=4a093979a3c2d02de2fbc00549a32771007f2e78032c6faa5ecd2f7a9e152025
    ${_osect_populate_only})

# mbedTLS: curl's TLS library on Linux (macOS and Windows use the OS's).
# 3.6 is the long-term-support line that curl 8.13 supports.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    FetchContent_Declare(mbedtls
        URL      https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.7/mbedtls-3.6.7.tar.bz2
        URL_HASH SHA256=a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6
        ${_osect_populate_only})
    FetchContent_MakeAvailable(mbedtls)
endif()

# SQLite amalgamation: no CMake project; built by the top-level CMakeLists.
FetchContent_Declare(sqlite
    URL      https://www.sqlite.org/2025/sqlite-autoconf-3490100.tar.gz
    URL_HASH SHA256=106642d8ccb36c5f7323b64e4152e9b719f7c0215acf5bfeac3d5e7f97b59254
    ${_osect_populate_only})

# Parents before their nested libraries: extracting a parent replaces its
# whole source directory, including external/.
FetchContent_MakeAvailable(
    sdl3
    sdl3_image sdl3_image_libtiff
    sdl3_ttf sdl3_ttf_freetype sdl3_ttf_harfbuzz sdl3_ttf_plutosvg sdl3_ttf_plutovg
    zlib curl sqlite)

# MoltenVK: precompiled universal dylib bundled into the macOS .app so the
# default Vulkan backend works without a user-installed Vulkan runtime.
if(APPLE AND OSECT_ENABLE_PACKAGING)
    FetchContent_Declare(moltenvk
        URL      https://github.com/KhronosGroup/MoltenVK/releases/download/v1.3.0/MoltenVK-macos.tar
        URL_HASH SHA256=57b4184ded521b08a63e3642552ebef8b9e98c2f0bcffa49bd93bbe1301c173d
        ${_osect_populate_only})
    FetchContent_MakeAvailable(moltenvk)
endif()
