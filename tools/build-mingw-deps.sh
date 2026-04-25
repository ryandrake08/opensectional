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
    CFLAGS="-DSQLITE_ENABLE_RTREE=1" \
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

echo ""
echo "=== Done ==="
echo "Dependencies installed to: ${PREFIX}"
echo ""
echo "To build osect for Windows:"
echo "  cmake -B build-mingw -DCMAKE_TOOLCHAIN_FILE=mingw-w64-toolchain.cmake -DCMAKE_BUILD_TYPE=Release"
echo "  cmake --build build-mingw -j"
