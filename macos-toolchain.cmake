# CMake toolchain file for universal (arm64+x86_64) macOS distribution builds.
# Usage:
#   cmake -B build-universal -DCMAKE_TOOLCHAIN_FILE=macos-toolchain.cmake -DCMAKE_BUILD_TYPE=Release
#
# Requires universal static builds of SDL3, SDL3_image, SDL3_ttf, and SQLite3.
# Produce them with tools/build-macos-deps.sh, which installs them into
# macos-universal-deps/.
#
# This is a maintainer path. For local development, use the standard
# 'cmake -B build' flow without this toolchain.

# FORCE into the cache so the toolchain wins over any value CMake's Apple
# platform module initialized to default, and over any value left in a stale
# build dir from an earlier configure.
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Build architectures" FORCE)
# Apple Silicon was introduced in macOS 11 (Big Sur). Lower won't run.
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version" FORCE)

if(NOT MACOS_DEPS_PREFIX)
    if(DEFINED ENV{MACOS_DEPS_PREFIX})
        set(MACOS_DEPS_PREFIX "$ENV{MACOS_DEPS_PREFIX}")
    else()
        set(MACOS_DEPS_PREFIX "${CMAKE_CURRENT_LIST_DIR}/macos-universal-deps")
    endif()
endif()

# Search our universal static deps before any host-installed Homebrew/MacPorts copies.
list(PREPEND CMAKE_PREFIX_PATH "${MACOS_DEPS_PREFIX}")

# pkg-config (used for sqlite3) must look only at the universal-deps prefix.
set(ENV{PKG_CONFIG_LIBDIR} "${MACOS_DEPS_PREFIX}/lib/pkgconfig")

# libcurl in the universal-deps prefix is libcurl.a; pull its
# Libs.private (SecureTransport frameworks, openssl/zlib/idn2/…)
# at link time.
set(OSECT_CURL_STATIC ON CACHE BOOL "Link libcurl statically" FORCE)
