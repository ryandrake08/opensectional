# Pinned tools for CPack's AppImage generator, downloaded into
# <build>/_deps/appimage/ for the build host's architecture (x86_64 or
# aarch64). Included from the top-level CMakeLists.txt's Linux packaging
# block; sets CPACK_APPIMAGE_TOOL_EXECUTABLE, CPACK_APPIMAGE_PATCHELF_EXECUTABLE
# and CPACK_APPIMAGE_RUNTIME_FILE.
#
# appimagetool ships as an AppImage; it is unpacked once with
# --appimage-extract (which needs no FUSE) and run from the unpacked tree.
# The runtime embedded in the generated AppImage is pinned too, instead of
# appimagetool fetching the latest one at packaging time.

string(TOLOWER "${CMAKE_HOST_SYSTEM_PROCESSOR}" _appimage_arch)
if(_appimage_arch MATCHES "^(x86_64|amd64)$")
    set(_appimage_arch x86_64)
    set(_appimagetool_hash ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0)
    set(_runtime_hash      2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d)
    set(_patchelf_hash     a6818fef80128fb354423234ecacdcca3e993913d774e5d8346bc63f70fed4cf)
elseif(_appimage_arch MATCHES "^(aarch64|arm64)$")
    set(_appimage_arch aarch64)
    set(_appimagetool_hash f0837e7448a0c1e4e650a93bb3e85802546e60654ef287576f46c71c126a9158)
    set(_runtime_hash      00cbdfcf917cc6c0ff6d3347d59e0ca1f7f45a6df1a428a0d6d8a78664d87444)
    set(_patchelf_hash     a2f8f5add5910a521d35062adf2c9f55d75b65ae5508d290758787004054e702)
else()
    message(FATAL_ERROR "AppImage packaging supports x86_64 and aarch64 build hosts, not ${CMAKE_HOST_SYSTEM_PROCESSOR}")
endif()

set(_appimage_dir "${CMAKE_BINARY_DIR}/_deps/appimage")

# Download url to dest unless dest already has the expected SHA-256.
function(_appimage_download url hash dest)
    if(EXISTS "${dest}")
        file(SHA256 "${dest}" _have)
        if(_have STREQUAL hash)
            return()
        endif()
    endif()
    message(STATUS "Downloading ${url}")
    file(DOWNLOAD "${url}" "${dest}" EXPECTED_HASH SHA256=${hash} STATUS _status)
    list(GET _status 0 _code)
    if(NOT _code EQUAL 0)
        message(FATAL_ERROR "Download failed: ${url}: ${_status}")
    endif()
endfunction()

_appimage_download(
    "https://github.com/AppImage/appimagetool/releases/download/1.9.1/appimagetool-${_appimage_arch}.AppImage"
    ${_appimagetool_hash} "${_appimage_dir}/appimagetool.AppImage")
_appimage_download(
    "https://github.com/AppImage/type2-runtime/releases/download/20251108/runtime-${_appimage_arch}"
    ${_runtime_hash} "${_appimage_dir}/runtime")
_appimage_download(
    "https://github.com/NixOS/patchelf/releases/download/0.19.1/patchelf-0.19.1-${_appimage_arch}.tar.gz"
    ${_patchelf_hash} "${_appimage_dir}/patchelf.tar.gz")

# Unpack when missing or older than the download it came from.
if(NOT EXISTS "${_appimage_dir}/appimagetool/AppRun"
   OR "${_appimage_dir}/appimagetool.AppImage" IS_NEWER_THAN "${_appimage_dir}/appimagetool/AppRun")
    file(REMOVE_RECURSE "${_appimage_dir}/appimagetool" "${_appimage_dir}/squashfs-root")
    file(CHMOD "${_appimage_dir}/appimagetool.AppImage"
        PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
    execute_process(
        COMMAND "${_appimage_dir}/appimagetool.AppImage" --appimage-extract
        WORKING_DIRECTORY "${_appimage_dir}"
        OUTPUT_QUIET
        RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "Unpacking appimagetool failed (rc=${_rc})")
    endif()
    file(RENAME "${_appimage_dir}/squashfs-root" "${_appimage_dir}/appimagetool")
endif()
if(NOT EXISTS "${_appimage_dir}/patchelf/bin/patchelf"
   OR "${_appimage_dir}/patchelf.tar.gz" IS_NEWER_THAN "${_appimage_dir}/patchelf/bin/patchelf")
    file(REMOVE_RECURSE "${_appimage_dir}/patchelf")
    file(ARCHIVE_EXTRACT INPUT "${_appimage_dir}/patchelf.tar.gz" DESTINATION "${_appimage_dir}/patchelf")
endif()

set(CPACK_APPIMAGE_TOOL_EXECUTABLE     "${_appimage_dir}/appimagetool/AppRun")
set(CPACK_APPIMAGE_PATCHELF_EXECUTABLE "${_appimage_dir}/patchelf/bin/patchelf")
set(CPACK_APPIMAGE_RUNTIME_FILE        "${_appimage_dir}/runtime")
