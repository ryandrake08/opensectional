# CMake toolchain file for universal (arm64+x86_64) macOS distribution builds.
# Selected by the macos-vendored and macos-package presets in
# CMakePresets.json. Contributor builds use the release / debug /
# relwithdebinfo presets with Homebrew or MacPorts dependencies instead.

# FORCE into the cache so the toolchain wins over any value CMake's Apple
# platform module initialized to default, and over any value left in a stale
# build dir from an earlier configure.
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Build architectures" FORCE)
# Apple Silicon was introduced in macOS 11 (Big Sur). Lower won't run.
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version" FORCE)

# Vendored libraries (curl, SDL, SDL_image) contain translation units that
# compile to nothing on macOS (other platforms' backends), and Apple's ranlib
# warns once per empty object per architecture. Silence that one diagnostic.
# CMake only supplies its default archive-finish rule when none is defined.
foreach(_lang C CXX OBJC OBJCXX)
    set(CMAKE_${_lang}_ARCHIVE_FINISH "<CMAKE_RANLIB> -no_warning_for_no_symbols <TARGET>")
endforeach()
