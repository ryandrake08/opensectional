#!/bin/bash
# Build universal (arm64+x86_64) static dependencies for macOS distribution
# builds. Produces SDL3, SDL3_image, SDL3_ttf, and SQLite3 in
# macos-universal-deps/, ready to be linked statically into a self-contained
# universal osect binary.
#
# This is a maintainer path. Local development should use the native build
# (cmake -B build) with Homebrew/MacPorts dylibs — see README.
#
# Requires: Xcode 12+ (for arm64 target), CMake, git, curl, GNU/BSD make.
# Does not assume Homebrew or MacPorts — uses only Xcode-provided clang plus
# whatever CMake/git/curl the user has installed.
set -euo pipefail

PROJECTDIR="$(cd "$(dirname "$0")/.." && pwd)"
PREFIX="${PROJECTDIR}/macos-universal-deps"
BUILDDIR="${PROJECTDIR}/macos-universal-deps-build"
DEPLOYMENT_TARGET=11.0
ARCH_FLAGS="-arch arm64 -arch x86_64"
JOBS=$(sysctl -n hw.ncpu)

# Versions (mirror tools/build-mingw-deps.sh)
SDL3_TAG=release-3.4.4
SDL3_IMAGE_TAG=release-3.4.2
SDL3_TTF_TAG=release-3.2.2
SQLITE_YEAR=2025
SQLITE_URL="https://www.sqlite.org/${SQLITE_YEAR}/sqlite-autoconf-3490100.tar.gz"
ZLIB_VERSION=1.3.1
ZLIB_URL="https://github.com/madler/zlib/releases/download/v${ZLIB_VERSION}/zlib-${ZLIB_VERSION}.tar.gz"
CURL_VERSION=8.13.0
CURL_URL="https://curl.se/download/curl-${CURL_VERSION}.tar.gz"
# MoltenVK ships precompiled universal (arm64+x86_64) binaries on its
# GitHub releases page, which avoids dragging in their large fetched
# dependency tree (SPIRV-Cross, glslang, SPIRV-Tools, ...) just to build
# a dylib. Update the tag to bump the bundled version.
MOLTENVK_TAG=v1.3.0
MOLTENVK_TAR_URL="https://github.com/KhronosGroup/MoltenVK/releases/download/${MOLTENVK_TAG}/MoltenVK-macos.tar"

DEPS_TOOLCHAIN="${BUILDDIR}/toolchain.cmake"

echo "=== Building macOS universal (arm64+x86_64) dependencies ==="
echo "  Prefix:    ${PREFIX}"
echo "  Build dir: ${BUILDDIR}"
echo "  Min OS:    ${DEPLOYMENT_TARGET}"
echo ""

mkdir -p "${BUILDDIR}" "${PREFIX}"

cat > "${DEPS_TOOLCHAIN}" <<TCEOF
# CACHE STRING ... FORCE is required (not just plain \`set()\`): some
# upstream CMakeLists (notably SDL3 3.4.x) read CMAKE_OSX_ARCHITECTURES
# from the cache or fall back to host arch when the cache entry is empty.
# A plain \`set()\` in the toolchain file is local to the toolchain scope
# and doesn't populate the cache, so universal builds silently degrade
# to single-arch (host).
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Build architectures for macOS" FORCE)
set(CMAKE_OSX_DEPLOYMENT_TARGET ${DEPLOYMENT_TARGET} CACHE STRING "Minimum macOS deployment target" FORCE)
# Prefer our cross-built deps over any host-installed Homebrew/MacPorts copies.
set(CMAKE_PREFIX_PATH ${PREFIX})
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
    -DSDLIMAGE_VENDORED=ON \
    -DSDLIMAGE_AVIF=OFF \
    -DSDLIMAGE_WEBP=OFF \
    -DPNG_HARDWARE_OPTIMIZATIONS=OFF
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
# Compile the amalgamation directly with -arch flags rather than going through
# SQLite's autoconf/libtool, which doesn't handle universal builds — libtool
# strips -arch flags during the archive step and produces a single-arch
# sqlite3.o that ld then rejects with "not a mach-o file".
echo "--- SQLite3 ---"
if [ ! -d "${BUILDDIR}/sqlite" ]; then
    mkdir -p "${BUILDDIR}/sqlite"
    curl -L "${SQLITE_URL}" | tar xz -C "${BUILDDIR}/sqlite" --strip-components=1
fi
cd "${BUILDDIR}/sqlite"
# RTREE = spatial bbox queries; FTS5 = the search index built by
# tools/build_search.py. Both must be compiled in — the amalgamation
# leaves them off by default.
#
# Re-run safety: remove prior outputs first. clang -c will overwrite the
# .o, but `ar rcs` chokes on an existing fat archive (Apple ar can't
# modify universal archives). Using libtool sidesteps that limitation
# regardless, but the explicit rm also keeps a half-built tree from
# polluting subsequent runs.
rm -f sqlite3.o libsqlite3.a
clang ${ARCH_FLAGS} \
    -mmacosx-version-min=${DEPLOYMENT_TARGET} \
    -DSQLITE_ENABLE_RTREE=1 \
    -DSQLITE_ENABLE_FTS5=1 \
    -O2 \
    -c sqlite3.c -o sqlite3.o
# Apple's libtool (not GNU libtool) is the canonical way to assemble a
# static archive on macOS — it understands universal slices and rewrites
# the output atomically rather than failing on existing fat archives.
libtool -static -o libsqlite3.a sqlite3.o
mkdir -p "${PREFIX}/include" "${PREFIX}/lib/pkgconfig"
cp sqlite3.h "${PREFIX}/include/"
cp libsqlite3.a "${PREFIX}/lib/"
SQLITE_VERSION=$(awk '/^#define SQLITE_VERSION / {gsub(/"/,"",$3); print $3; exit}' sqlite3.h)
cat > "${PREFIX}/lib/pkgconfig/sqlite3.pc" <<PCEOF
prefix=\${pcfiledir}/../..
exec_prefix=\${prefix}
libdir=\${exec_prefix}/lib
includedir=\${prefix}/include

Name: SQLite
Description: SQL database engine
Version: ${SQLITE_VERSION}
Libs: -L\${libdir} -lsqlite3
Cflags: -I\${includedir}
PCEOF
cd -

# ---------- zlib ----------
# Built into macos-universal-deps for the same reason curl is: the
# universal binary needs a fat libz.a, and MacPorts/Homebrew ship
# single-arch copies that would poison the link. zlib's own configure
# can produce a universal static lib when handed CC with `-arch arm64
# -arch x86_64`.
echo "--- zlib (${ZLIB_VERSION}) ---"
if [ ! -d "${BUILDDIR}/zlib" ]; then
    mkdir -p "${BUILDDIR}/zlib"
    curl -L "${ZLIB_URL}" | tar xz -C "${BUILDDIR}/zlib" --strip-components=1
fi
cd "${BUILDDIR}/zlib"
if [ ! -f .configured ]; then
    CC="clang ${ARCH_FLAGS} -mmacosx-version-min=${DEPLOYMENT_TARGET}" \
        ./configure --prefix="${PREFIX}" --static
    touch .configured
fi
make -j "${JOBS}"
make install
cd -

# ---------- libcurl ----------
# SecureTransport is deprecated by Apple but still functional through
# at least macOS 14, and avoids dragging OpenSSL into the static deps
# tree. zlib is consumed from the universal static copy we just built
# above — system libz lives at /usr/lib but MacPorts/Homebrew copies
# in pkg-config search paths would otherwise pollute the link with
# single-arch -L hints.
#
# Universal-build trick: pass `-arch arm64 -arch x86_64` via CC so each
# autoconf conftest is compiled fat. Configure's feature probes don't
# look at slice-specific output, so a single configure run produces a
# universal libcurl.a.
echo "--- libcurl (${CURL_VERSION}) ---"
if [ ! -d "${BUILDDIR}/curl" ]; then
    mkdir -p "${BUILDDIR}/curl"
    curl -L "${CURL_URL}" | tar xz -C "${BUILDDIR}/curl" --strip-components=1
fi
cd "${BUILDDIR}/curl"
if [ ! -f .configured ]; then
    PKG_CONFIG_LIBDIR="${PREFIX}/lib/pkgconfig" \
    CC="clang ${ARCH_FLAGS}" \
    CFLAGS="-mmacosx-version-min=${DEPLOYMENT_TARGET}" \
    LDFLAGS="-mmacosx-version-min=${DEPLOYMENT_TARGET}" \
    ./configure \
        --prefix="${PREFIX}" \
        --disable-shared --enable-static \
        --with-secure-transport \
        --with-zlib="${PREFIX}" \
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

# ---------- MoltenVK ----------
# Bundle MoltenVK into the .app's Contents/Frameworks/ at install time so
# the Vulkan backend works on a fresh Mac without the user installing the
# Vulkan SDK or Homebrew. SDL3 searches @executable_path/../Frameworks/
# libMoltenVK.dylib first when loading Vulkan on macOS, so once the dylib
# is in that location no app code or env-var hackery is needed.
echo "--- MoltenVK (${MOLTENVK_TAG}) ---"
if [ ! -d "${BUILDDIR}/MoltenVK" ]; then
    mkdir -p "${BUILDDIR}/MoltenVK"
    curl -L "${MOLTENVK_TAR_URL}" | tar x -C "${BUILDDIR}/MoltenVK"
fi
# Layout has shifted across MoltenVK releases (v1.2.x used .xcframework/
# macos-arm64_x86_64/; v1.3.x uses dynamic/dylib/macOS/). Just look for the
# .dylib anywhere in the tree — the file extension distinguishes the
# dynamic library we want from the static .a we don't.
MVK_DYLIB=$(find "${BUILDDIR}/MoltenVK" -name 'libMoltenVK.dylib' -type f | head -1)
MVK_LICENSE=$(find "${BUILDDIR}/MoltenVK" -name 'LICENSE' -type f | head -1)
if [ -z "${MVK_DYLIB}" ] || [ ! -f "${MVK_DYLIB}" ]; then
    echo "ERROR: MoltenVK universal dylib not found in extracted tarball" >&2
    exit 1
fi
# Sanity-check the slice list before installing.
if ! lipo -info "${MVK_DYLIB}" | grep -q 'arm64' || ! lipo -info "${MVK_DYLIB}" | grep -q 'x86_64'; then
    echo "ERROR: MoltenVK dylib is not universal: $(lipo -info "${MVK_DYLIB}")" >&2
    exit 1
fi
mkdir -p "${PREFIX}/lib" "${PREFIX}/share/doc/MoltenVK"
cp "${MVK_DYLIB}" "${PREFIX}/lib/libMoltenVK.dylib"
if [ -n "${MVK_LICENSE}" ] && [ -f "${MVK_LICENSE}" ]; then
    cp "${MVK_LICENSE}" "${PREFIX}/share/doc/MoltenVK/LICENSE"
fi

# Make installed .pc files relocatable so the deps tree survives a rename of
# its parent path. Without this, prefix= is the absolute install path at build
# time and pkg-config emits stale -I/-L flags after any move.
# macOS uses BSD sed, which requires -i with an explicit suffix.
for pc in "${PREFIX}/lib/pkgconfig/"*.pc; do
    [ -f "$pc" ] || continue
    sed -i.bak 's|^prefix=.*|prefix=${pcfiledir}/../..|' "$pc"
    rm -f "$pc.bak"
done

echo ""
echo "=== Done ==="
echo "Universal dependencies installed to: ${PREFIX}"
echo ""
echo "To build a universal osect binary:"
echo "  cmake -B build-universal -DCMAKE_TOOLCHAIN_FILE=macos-toolchain.cmake -DCMAKE_BUILD_TYPE=Release"
echo "  cmake --build build-universal -j"
