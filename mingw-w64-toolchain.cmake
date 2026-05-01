# CMake toolchain file for cross-compiling to Windows using MinGW-w64
# Usage: cmake -B build-mingw -DCMAKE_TOOLCHAIN_FILE=mingw-w64-toolchain.cmake -DCMAKE_BUILD_TYPE=Release

# Target system
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# Toolchain prefix
set(TOOLCHAIN_PREFIX x86_64-w64-mingw32)

# Compilers
set(CMAKE_C_COMPILER ${TOOLCHAIN_PREFIX}-gcc)
set(CMAKE_CXX_COMPILER ${TOOLCHAIN_PREFIX}-g++)
set(CMAKE_RC_COMPILER ${TOOLCHAIN_PREFIX}-windres)

# Cross-compiled dependency prefix (SDL3, SQLite3, etc.)
# Override with: cmake -DMINGW_DEPS_PREFIX=/path/to/deps ...
if(NOT MINGW_DEPS_PREFIX)
    if(DEFINED ENV{MINGW_DEPS_PREFIX})
        set(MINGW_DEPS_PREFIX "$ENV{MINGW_DEPS_PREFIX}")
    else()
        set(MINGW_DEPS_PREFIX "${CMAKE_CURRENT_LIST_DIR}/mingw-deps")
    endif()
endif()

set(CMAKE_PREFIX_PATH "${MINGW_DEPS_PREFIX}")

# Find root paths for cross-compilation
set(CMAKE_FIND_ROOT_PATH
    /usr/${TOOLCHAIN_PREFIX}
    ${MINGW_DEPS_PREFIX}
)

# Search mode settings
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# pkg-config must search the cross-compiled prefix, not the host
set(ENV{PKG_CONFIG_LIBDIR} "${MINGW_DEPS_PREFIX}/lib/pkgconfig")
set(ENV{PKG_CONFIG_SYSROOT_DIR} "${MINGW_DEPS_PREFIX}")

# Static linking for MinGW runtime
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static-libgcc -static-libstdc++")

# libcurl in the mingw-deps prefix is libcurl.a; pull its
# Libs.private (Schannel, winsock, zlib) at link time, and add
# -DCURL_STATICLIB so the curl headers stop decorating with
# __declspec(dllimport).
set(OSECT_CURL_STATIC ON CACHE BOOL "Link libcurl statically" FORCE)
