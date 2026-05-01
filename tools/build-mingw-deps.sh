#!/bin/bash
# Cross-compile dependencies for Windows (MinGW-w64)
# Builds SDL3, SDL3_image, SDL3_ttf, and SQLite3 into mingw-deps/
set -euo pipefail

PROJECTDIR="$(cd "$(dirname "$0")/.." && pwd)"
PREFIX="${PROJECTDIR}/mingw-deps"
BUILDDIR="${PROJECTDIR}/mingw-deps-build"
TOOLCHAIN_PREFIX=x86_64-w64-mingw32
JOBS=$(nproc)

# Versions
SDL3_TAG=release-3.4.4
SDL3_IMAGE_TAG=release-3.4.2
SDL3_TTF_TAG=release-3.2.2
SQLITE_YEAR=2025
SQLITE_URL="https://www.sqlite.org/${SQLITE_YEAR}/sqlite-autoconf-3490100.tar.gz"
ZLIB_VERSION=1.3.1
ZLIB_URL="https://github.com/madler/zlib/releases/download/v${ZLIB_VERSION}/zlib-${ZLIB_VERSION}.tar.gz"
CURL_VERSION=8.13.0
CURL_URL="https://curl.se/download/curl-${CURL_VERSION}.tar.gz"

# Write a minimal toolchain file for dependency builds
DEPS_TOOLCHAIN="${BUILDDIR}/toolchain.cmake"

echo "=== Building MinGW-w64 dependencies ==="
echo "  Prefix: ${PREFIX}"
echo "  Build dir: ${BUILDDIR}"
echo ""

mkdir -p "${BUILDDIR}" "${PREFIX}"

cat > "${DEPS_TOOLCHAIN}" <<TCEOF
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER ${TOOLCHAIN_PREFIX}-gcc)
set(CMAKE_CXX_COMPILER ${TOOLCHAIN_PREFIX}-g++)
set(CMAKE_RC_COMPILER ${TOOLCHAIN_PREFIX}-windres)
set(CMAKE_FIND_ROOT_PATH /usr/${TOOLCHAIN_PREFIX} ${PREFIX})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
TCEOF

# ---------- SDL3 ----------
echo "--- SDL3 (${SDL3_TAG}) ---"
if [ ! -d "${BUILDDIR}/SDL" ]; then
    git clone --depth 1 --branch "${SDL3_TAG}" https://github.com/libsdl-org/SDL.git "${BUILDDIR}/SDL"
fi
cmake -S "${BUILDDIR}/SDL" -B "${BUILDDIR}/SDL/build" \
    -DCMAKE_TOOLCHAIN_FILE="${DEPS_TOOLCHAIN}" \
    -DCMAKE_INSTALL_PREFIX="${PREFIX}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DSDL_SHARED=OFF \
    -DSDL_STATIC=ON \
    -DSDL_VULKAN=ON
cmake --build "${BUILDDIR}/SDL/build" -j "${JOBS}"
cmake --install "${BUILDDIR}/SDL/build"

# ---------- SDL3_image ----------
echo "--- SDL3_image (${SDL3_IMAGE_TAG}) ---"
if [ ! -d "${BUILDDIR}/SDL_image" ]; then
    git clone --depth 1 --branch "${SDL3_IMAGE_TAG}" https://github.com/libsdl-org/SDL_image.git "${BUILDDIR}/SDL_image"
    git -C "${BUILDDIR}/SDL_image" submodule update --init --depth 1
fi
cmake -S "${BUILDDIR}/SDL_image" -B "${BUILDDIR}/SDL_image/build" \
    -DCMAKE_TOOLCHAIN_FILE="${DEPS_TOOLCHAIN}" \
    -DCMAKE_INSTALL_PREFIX="${PREFIX}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SHARED_LIBS=OFF \
    -DSDLIMAGE_VENDORED=ON
cmake --build "${BUILDDIR}/SDL_image/build" -j "${JOBS}"
cmake --install "${BUILDDIR}/SDL_image/build"

# ---------- SDL3_ttf ----------
echo "--- SDL3_ttf (${SDL3_TTF_TAG}) ---"
if [ ! -d "${BUILDDIR}/SDL_ttf" ]; then
    git clone --depth 1 --branch "${SDL3_TTF_TAG}" https://github.com/libsdl-org/SDL_ttf.git "${BUILDDIR}/SDL_ttf"
    git -C "${BUILDDIR}/SDL_ttf" submodule update --init --depth 1
fi
cmake -S "${BUILDDIR}/SDL_ttf" -B "${BUILDDIR}/SDL_ttf/build" \
    -DCMAKE_TOOLCHAIN_FILE="${DEPS_TOOLCHAIN}" \
    -DCMAKE_INSTALL_PREFIX="${PREFIX}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SHARED_LIBS=OFF \
    -DSDLTTF_VENDORED=ON
cmake --build "${BUILDDIR}/SDL_ttf/build" -j "${JOBS}"
cmake --install "${BUILDDIR}/SDL_ttf/build"

# ---------- SQLite3 ----------
echo "--- SQLite3 ---"
if [ ! -d "${BUILDDIR}/sqlite" ]; then
    mkdir -p "${BUILDDIR}/sqlite"
    curl -L "${SQLITE_URL}" | tar xz -C "${BUILDDIR}/sqlite" --strip-components=1
fi
cd "${BUILDDIR}/sqlite"
if [ ! -f Makefile ] || [ ! -f .configured ]; then
    # RTREE = spatial bbox queries; FTS5 = the search index built by
    # tools/build_search.py. Both must be compiled in — the amalgamation
    # leaves them off by default.
    CFLAGS="-DSQLITE_ENABLE_RTREE=1 -DSQLITE_ENABLE_FTS5=1" \
    ./configure \
        --host=${TOOLCHAIN_PREFIX} \
        --prefix="${PREFIX}" \
        --enable-static \
        --disable-shared
    touch .configured
fi
make -j "${JOBS}"
make install
cd -

# ---------- zlib ----------
# Required by libcurl for transparent gzip decompression of HTTP
# responses. The cross-compile target has no system zlib, so build a
# static one into the deps prefix.
echo "--- zlib (${ZLIB_VERSION}) ---"
if [ ! -d "${BUILDDIR}/zlib" ]; then
    mkdir -p "${BUILDDIR}/zlib"
    curl -L "${ZLIB_URL}" | tar xz -C "${BUILDDIR}/zlib" --strip-components=1
fi
cmake -S "${BUILDDIR}/zlib" -B "${BUILDDIR}/zlib/build" \
    -DCMAKE_TOOLCHAIN_FILE="${DEPS_TOOLCHAIN}" \
    -DCMAKE_INSTALL_PREFIX="${PREFIX}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DZLIB_BUILD_EXAMPLES=OFF \
    -DBUILD_SHARED_LIBS=OFF
cmake --build "${BUILDDIR}/zlib/build" -j "${JOBS}"
cmake --install "${BUILDDIR}/zlib/build"
# zlib's CMakeLists installs both the static and import-lib variants
# even with BUILD_SHARED_LIBS=OFF. Strip the dll/import-lib leftovers
# so curl's configure picks up the static archive unambiguously.
rm -f "${PREFIX}/lib/libzlib.dll.a" "${PREFIX}/lib/zlib.lib" \
      "${PREFIX}/bin/zlib1.dll" 2>/dev/null || true
# zlib's CMake build on MinGW installs `libzlibstatic.a` rather than
# the canonical `libz.a` — the OUTPUT_NAME=z rename only fires when
# CMake's UNIX flag is set, and a Windows cross-compile target has
# UNIX=FALSE. zlib.pc still says `-lz` though, so consumers (curl,
# osect itself) look for libz.a. Copy the static archive to that name
# so `-lz` resolves.
if [ -f "${PREFIX}/lib/libzlibstatic.a" ] && [ ! -f "${PREFIX}/lib/libz.a" ]; then
    cp "${PREFIX}/lib/libzlibstatic.a" "${PREFIX}/lib/libz.a"
fi

# ---------- libcurl ----------
# Build with Schannel (Windows' native TLS) so we don't drag OpenSSL
# into the static-linked binary. Trims the protocol set down to what
# we actually use (HTTPS); everything else is disabled to keep the
# static .a small.
echo "--- libcurl (${CURL_VERSION}) ---"
if [ ! -d "${BUILDDIR}/curl" ]; then
    mkdir -p "${BUILDDIR}/curl"
    curl -L "${CURL_URL}" | tar xz -C "${BUILDDIR}/curl" --strip-components=1
fi
cd "${BUILDDIR}/curl"
if [ ! -f .configured ]; then
    # MinGW configure tests for individual socket functions
    # (select/recv/etc.) link without inheriting earlier checks'
    # cached lib list, so each one fails unless -lws2_32 is present
    # in LIBS from the start. Schannel itself pulls in -lcrypt32
    # and -lbcrypt for cert handling; declare them up front too so
    # configure's TLS detection succeeds.
    LIBS="-lws2_32 -lcrypt32 -lbcrypt" \
    PKG_CONFIG_PATH="${PREFIX}/lib/pkgconfig" \
    CPPFLAGS="-I${PREFIX}/include" \
    LDFLAGS="-L${PREFIX}/lib" \
    ./configure \
        --host=${TOOLCHAIN_PREFIX} \
        --prefix="${PREFIX}" \
        --disable-shared --enable-static \
        --with-schannel \
        --with-zlib \
        --disable-ldap --disable-ldaps \
        --disable-rtsp --disable-dict --disable-telnet \
        --disable-tftp --disable-pop3 --disable-imap \
        --disable-smtp --disable-smb --disable-gopher \
        --disable-mqtt --disable-manual \
        --without-libpsl --without-libidn2 \
        --without-nghttp2 --without-zstd --without-brotli \
        --without-libssh2 --without-librtmp
    touch .configured
fi
make -j "${JOBS}"
make install
cd -

# Make installed .pc files relocatable so the deps tree survives a rename of
# its parent path. Without this, prefix= is the absolute install path at build
# time and pkg-config emits stale -I/-L flags after any move.
for pc in "${PREFIX}/lib/pkgconfig/"*.pc; do
    [ -f "$pc" ] || continue
    sed -i 's|^prefix=.*|prefix=${pcfiledir}/../..|' "$pc"
done

echo ""
echo "=== Done ==="
echo "Dependencies installed to: ${PREFIX}"
echo ""
echo "To build osect for Windows:"
echo "  cmake -B build-mingw -DCMAKE_TOOLCHAIN_FILE=mingw-w64-toolchain.cmake -DCMAKE_BUILD_TYPE=Release"
echo "  cmake --build build-mingw -j"
