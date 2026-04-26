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

DEPS_TOOLCHAIN="${BUILDDIR}/toolchain.cmake"

echo "=== Building macOS universal (arm64+x86_64) dependencies ==="
echo "  Prefix:    ${PREFIX}"
echo "  Build dir: ${BUILDDIR}"
echo "  Min OS:    ${DEPLOYMENT_TARGET}"
echo ""

mkdir -p "${BUILDDIR}" "${PREFIX}"

cat > "${DEPS_TOOLCHAIN}" <<TCEOF
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64")
set(CMAKE_OSX_DEPLOYMENT_TARGET ${DEPLOYMENT_TARGET})
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
clang ${ARCH_FLAGS} \
    -mmacosx-version-min=${DEPLOYMENT_TARGET} \
    -DSQLITE_ENABLE_RTREE=1 \
    -O2 \
    -c sqlite3.c -o sqlite3.o
ar rcs libsqlite3.a sqlite3.o
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
