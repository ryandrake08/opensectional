# Pinned prebuilt dxc (HLSL → DXIL) for the D3D12 backend. Included from
# shaders/CMakeLists.txt when the target is Windows and OSECT_ENABLE_D3D12 is
# ON. Microsoft publishes release binaries for Linux x86_64 (the cross-build
# host) and Windows x64 / arm64 (native MSYS2 builds); on those hosts the
# archive is fetched into <build>/_deps/ and OSECT_DXC is set to its dxc.
# Other hosts (macOS, Linux arm64) have no prebuilt binary; OSECT_DXC stays
# unset and the caller falls back to a dxc found on PATH / $VULKAN_SDK/bin.
#
# dxc computes the DXIL validation hash itself, so the separate validator
# library (dxil.dll / libdxil.so) isn't needed for D3D12 to accept shaders.

include(FetchContent)

# Silence the extracted-timestamp warning for URL downloads (CMake >= 3.24).
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()

set(_dxc_release "https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1.9.2609")
string(TOLOWER "${CMAKE_HOST_SYSTEM_PROCESSOR}" _dxc_host_cpu)
unset(_dxc_url)

if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux" AND _dxc_host_cpu MATCHES "^(x86_64|amd64)$")
    set(_dxc_url  "${_dxc_release}/linux_dxc_2026_09_28.x86_x64.tar.gz")
    set(_dxc_hash 96faadc7f5c282d2ffda49804beb4c3ee38127bc252b723234e3c5cdf7aa39a1)
    set(_dxc_exe  bin/dxc)
elseif(CMAKE_HOST_WIN32 AND _dxc_host_cpu MATCHES "^(x86_64|amd64)$")
    set(_dxc_url  "${_dxc_release}/dxc_2026_09_29.zip")
    set(_dxc_hash ad31b1fc8443175d204f77a611fdb3ef2ec42759bdc2f1167368de24a4a7e7f1)
    set(_dxc_exe  bin/x64/dxc.exe)
elseif(CMAKE_HOST_WIN32 AND _dxc_host_cpu MATCHES "^(arm64|aarch64)$")
    set(_dxc_url  "${_dxc_release}/dxc_2026_09_29.zip")
    set(_dxc_hash ad31b1fc8443175d204f77a611fdb3ef2ec42759bdc2f1167368de24a4a7e7f1)
    set(_dxc_exe  bin/arm64/dxc.exe)
endif()

if(DEFINED _dxc_url)
    # SOURCE_SUBDIR points at a directory that doesn't exist: populate only.
    FetchContent_Declare(dxc
        URL      "${_dxc_url}"
        URL_HASH SHA256=${_dxc_hash}
        SOURCE_SUBDIR _osect_populate_only)
    FetchContent_MakeAvailable(dxc)
    set(OSECT_DXC "${dxc_SOURCE_DIR}/${_dxc_exe}")
endif()
