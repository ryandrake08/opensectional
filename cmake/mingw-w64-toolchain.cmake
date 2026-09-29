# CMake toolchain file for cross-compiling to Windows using MinGW-w64.
# Selected by the mingw-vendored and mingw-package presets in
# CMakePresets.json. tools/build-mingw-package.sh runs the mingw-package
# preset after handling `git submodule update --init` and the SQLite tarball
# download. MSYS2 contributors building natively on Windows use the release /
# debug / relwithdebinfo presets with mingw-w64-x86_64-* pacman packages
# instead.

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
