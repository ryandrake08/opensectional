# CMake toolchain file for cross-compiling to Windows using MinGW-w64.
# Used by tools/build-mingw-package.sh to produce the NSIS installer; that
# script also handles `git submodule update --init` and the SQLite tarball
# download, then invokes:
#   cmake -B build-mingw-package \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-toolchain.cmake \
#         -DCMAKE_BUILD_TYPE=Release \
#         -DOSECT_VENDOR_DEPS=ON
# MSYS2 contributors building locally use the default `cmake -B build` flow
# with mingw-w64-x86_64-SDL3* / mingw-w64-x86_64-curl pacman packages.

# Target system
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# Toolchain prefix
set(TOOLCHAIN_PREFIX x86_64-w64-mingw32)

# Compilers
set(CMAKE_C_COMPILER ${TOOLCHAIN_PREFIX}-gcc)
set(CMAKE_CXX_COMPILER ${TOOLCHAIN_PREFIX}-g++)
set(CMAKE_RC_COMPILER ${TOOLCHAIN_PREFIX}-windres)

# Find root paths for cross-compilation
set(CMAKE_FIND_ROOT_PATH /usr/${TOOLCHAIN_PREFIX})

# Search mode settings
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Static linking for MinGW runtime
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static-libgcc -static-libstdc++")
