# Shim FindZLIB.cmake — short-circuits CMake's stock FindZLIB so curl's
# `find_package(ZLIB)` resolves against the vendored static zlib fetched
# by cmake/vendored_deps.cmake and added via add_subdirectory(). The real `zlibstatic` target
# (and our ZLIB::ZLIB alias) are created in the top-level CMakeLists
# before this module is consulted.
if(NOT TARGET zlibstatic)
    message(FATAL_ERROR "FindZLIB shim invoked but zlibstatic target is not defined. "
        "The vendored zlib must be add_subdirectory'd before any find_package(ZLIB).")
endif()

set(ZLIB_FOUND TRUE)
set(ZLIB_LIBRARIES zlibstatic)
set(ZLIB_INCLUDE_DIRS
    "${zlib_SOURCE_DIR}"
    "${zlib_BINARY_DIR}")

# Read the version directly from the vendored zlib.h so a version
# bump doesn't silently lie. Mirrors what CMake's stock FindZLIB does.
set(ZLIB_VERSION_STRING "")
set(_zlib_h "${zlib_SOURCE_DIR}/zlib.h")
if(EXISTS "${_zlib_h}")
    file(STRINGS "${_zlib_h}" _zlib_h_line REGEX "^#define ZLIB_VERSION \"[^\"]*\"$")
    if(_zlib_h_line MATCHES "ZLIB_VERSION \"(([0-9]+)\\.([0-9]+)(\\.([0-9]+)(\\.([0-9]+))?)?)")
        set(ZLIB_VERSION_STRING "${CMAKE_MATCH_1}")
    endif()
endif()
set(ZLIB_VERSION "${ZLIB_VERSION_STRING}")

if(NOT TARGET ZLIB::ZLIB)
    add_library(ZLIB::ZLIB ALIAS zlibstatic)
endif()
