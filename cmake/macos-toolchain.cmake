# CMake toolchain file for universal (arm64+x86_64) macOS distribution builds.
# Used by tools/build-macos-package.sh to produce the .dmg installer; that
# script also handles `git submodule update --init` and the MoltenVK / SQLite
# tarball downloads, then invokes:
#   cmake -B build-macos-package \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/macos-toolchain.cmake \
#         -DCMAKE_BUILD_TYPE=Release \
#         -DOSECT_VENDOR_DEPS=ON
# Native dev uses the default `cmake -B build` flow with Homebrew SDL3/curl.

# FORCE into the cache so the toolchain wins over any value CMake's Apple
# platform module initialized to default, and over any value left in a stale
# build dir from an earlier configure.
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Build architectures" FORCE)
# Apple Silicon was introduced in macOS 11 (Big Sur). Lower won't run.
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version" FORCE)
