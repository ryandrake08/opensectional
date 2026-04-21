cmake_minimum_required(VERSION 3.10)
project(nasrbrowse CXX)

# Package metadata (used by CPack on both platforms and by the macOS bundle)
set(MAJOR_VERSION "0")
set(MINOR_VERSION "1")
set(BUILD_VERSION "0")
set(APPLICATION_VERSION "${MAJOR_VERSION}.${MINOR_VERSION}.${BUILD_VERSION}")
set(BUNDLE_IDENTIFIER "org.existens.nasrbrowse")

# Export compile commands
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Build-type-specific compiler options (MinGW lacks ASan and gprof support)
if(MINGW)
    set(DEBUG_COMPILE_OPTIONS -O0 -g)
    set(RELWITHDEBINFO_COMPILE_OPTIONS -O2 -g)
else()
    set(DEBUG_COMPILE_OPTIONS -O0 -ggdb -fsanitize=address)
    set(RELWITHDEBINFO_COMPILE_OPTIONS -O2 -g -pg)
endif()

# Find SDL3 (system-installed)
find_package(SDL3 REQUIRED CONFIG)
find_package(SDL3_image REQUIRED CONFIG)
find_package(SDL3_ttf REQUIRED CONFIG)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SQLITE3 REQUIRED sqlite3)

# Third-party libraries
add_subdirectory(thirdparty/imgui)

# Tests (doctest, opt-in). Built as a separate binary with ctest integration.
option(BUILD_TESTING "Build unit tests" ON)
if(BUILD_TESTING)
    enable_testing()
    add_subdirectory(tests)
endif()

# Main executable
add_executable(nasrbrowse
    # Application sources
    src/ini_config.cpp
    src/geo_math.cpp
    src/map_view.cpp
    src/map_widget.cpp
    src/flight_route.cpp
    src/feature_builder.cpp
    src/feature_type.cpp
    src/feature_renderer.cpp
    src/label_renderer.cpp
    src/line_renderer.cpp
    src/main.cpp
    src/nasr_database.cpp
    src/render_context.cpp
    src/tile_loader.cpp
    src/chart_style.cpp
    src/tile_renderer.cpp
    src/ui_overlay.cpp
    src/ui_sectioned_list.cpp
)

# SDL C++ wrapper library
add_library(sdl_wrapper STATIC
    lib/sdl/buffer.cpp
    lib/sdl/command_buffer.cpp
    lib/sdl/copy_pass.cpp
    lib/sdl/depth_buffer.cpp
    lib/sdl/device.cpp
    lib/sdl/error.cpp
    lib/sdl/event.cpp
    lib/sdl/filesystem.cpp
    lib/sdl/font.cpp
    lib/sdl/instance.cpp
    lib/sdl/pipeline.cpp
    lib/sdl/render_pass.cpp
    lib/sdl/sampler.cpp
    lib/sdl/shader.cpp
    lib/sdl/surface.cpp
    lib/sdl/text.cpp
    lib/sdl/text_engine.cpp
    lib/sdl/texture.cpp
    lib/sdl/timer.cpp
    lib/sdl/transfer_buffer.cpp
    lib/sdl/types.cpp
    lib/sdl/window.cpp
)
target_compile_features(sdl_wrapper PRIVATE cxx_std_11)
target_include_directories(sdl_wrapper PUBLIC
    ${CMAKE_SOURCE_DIR}/lib
    ${CMAKE_SOURCE_DIR}/thirdparty/glm-1.0.1
    ${CMAKE_SOURCE_DIR}/thirdparty
)
target_link_libraries(sdl_wrapper PUBLIC SDL3::SDL3 SDL3_image::SDL3_image SDL3_ttf::SDL3_ttf)
target_compile_options(sdl_wrapper PRIVATE -Wall -Wextra)
target_compile_options(sdl_wrapper PRIVATE
    $<$<CONFIG:Debug>:${DEBUG_COMPILE_OPTIONS}>
    $<$<CONFIG:Release>:-O3>
    $<$<CONFIG:RelWithDebInfo>:${RELWITHDEBINFO_COMPILE_OPTIONS}>
)

# ImGui C++ wrapper library
add_library(imgui_wrapper STATIC lib/imgui/context.cpp)
target_compile_features(imgui_wrapper PRIVATE cxx_std_11)
target_include_directories(imgui_wrapper PRIVATE ${CMAKE_SOURCE_DIR}/lib)
target_link_libraries(imgui_wrapper PUBLIC sdl_wrapper imgui)
target_compile_options(imgui_wrapper PRIVATE -Wall -Wextra)
target_compile_options(imgui_wrapper PRIVATE
    $<$<CONFIG:Debug>:${DEBUG_COMPILE_OPTIONS}>
    $<$<CONFIG:Release>:-O3>
    $<$<CONFIG:RelWithDebInfo>:${RELWITHDEBINFO_COMPILE_OPTIONS}>
)

# SQLite C++ wrapper library
add_library(sqlite_wrapper STATIC
    lib/sqlite/database.cpp
    lib/sqlite/statement.cpp
)
target_compile_features(sqlite_wrapper PRIVATE cxx_std_11)
target_include_directories(sqlite_wrapper PUBLIC ${CMAKE_SOURCE_DIR}/lib)
target_link_libraries(sqlite_wrapper PUBLIC ${SQLITE3_LINK_LIBRARIES})
target_include_directories(sqlite_wrapper PUBLIC ${SQLITE3_INCLUDE_DIRS})
target_compile_options(sqlite_wrapper PRIVATE -Wall -Wextra)
target_compile_options(sqlite_wrapper PRIVATE
    $<$<CONFIG:Debug>:${DEBUG_COMPILE_OPTIONS}>
    $<$<CONFIG:Release>:-O3>
    $<$<CONFIG:RelWithDebInfo>:${RELWITHDEBINFO_COMPILE_OPTIONS}>
)

# Include directories
target_include_directories(nasrbrowse PRIVATE
    ${CMAKE_SOURCE_DIR}/src    # For #include "layer.hpp" etc.
    ${CMAKE_SOURCE_DIR}/thirdparty
)

# C++ standard
target_compile_features(nasrbrowse PRIVATE cxx_std_17)

# Compiler warnings (all build types)
target_compile_options(nasrbrowse PRIVATE -Wall -Wextra)

# Build-type-specific compiler options
target_compile_options(nasrbrowse PRIVATE
    $<$<CONFIG:Debug>:${DEBUG_COMPILE_OPTIONS}>
    $<$<CONFIG:Release>:-O3>
    $<$<CONFIG:RelWithDebInfo>:${RELWITHDEBINFO_COMPILE_OPTIONS}>
)

# Build-type-specific definitions
target_compile_definitions(nasrbrowse PRIVATE
    $<$<CONFIG:Debug>:DEBUG=1>
)

# Build-type-specific linker options
target_link_options(nasrbrowse PRIVATE
    $<$<CONFIG:Debug>:${DEBUG_COMPILE_OPTIONS}>
    $<$<CONFIG:RelWithDebInfo>:${RELWITHDEBINFO_COMPILE_OPTIONS}>
)

# Link libraries
target_link_libraries(nasrbrowse PRIVATE
    imgui_wrapper
    sqlite_wrapper
)

# Embed font as C header
find_program(XXD xxd REQUIRED)
set(FONT_SOURCE "${CMAKE_SOURCE_DIR}/thirdparty/fonts/NotoSans-Regular.ttf")
set(FONT_HEADER "${CMAKE_BINARY_DIR}/NotoSans_Regular_ttf.h")
add_custom_command(
    OUTPUT ${FONT_HEADER}
    COMMAND ${XXD} -i NotoSans-Regular.ttf > ${FONT_HEADER}
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/thirdparty/fonts"
    DEPENDS ${FONT_SOURCE}
    COMMENT "Embedding font: NotoSans-Regular.ttf"
    VERBATIM
)
add_custom_target(embedded_font DEPENDS ${FONT_HEADER})

# Shader compilation (Metal, SPIRV)
add_subdirectory(shaders)

# Add shader and font headers to include path
target_include_directories(nasrbrowse PRIVATE ${SHADER_INCLUDE_DIR} ${CMAKE_BINARY_DIR})

# Ensure shaders and font are built before the executable
add_dependencies(nasrbrowse shaders embedded_font)

# =============================================================================
# Installer / packaging
# =============================================================================
#
# Prebuilt assets that ship with the installer. They are not in source control;
# regenerate them via tools/ before running `cpack`.
set(BUNDLED_DB      "${CMAKE_SOURCE_DIR}/nasr.db")
set(BUNDLED_BASEMAP "${CMAKE_SOURCE_DIR}/basemap")
set(BUNDLED_INI     "${CMAKE_SOURCE_DIR}/nasrbrowse.ini")

set(ICON_PNG            "${CMAKE_SOURCE_DIR}/nasrbrowse.png")
set(INSTALLER_ICON_ICNS "${CMAKE_BINARY_DIR}/nasrbrowse.icns")
set(INSTALLER_ICON_ICO  "${CMAKE_BINARY_DIR}/nasrbrowse.ico")

# Platform-specific icon generation from the master PNG. Skipped silently
# if the required tool is missing — installer blocks below make the icon
# optional.
if(APPLE AND EXISTS "${ICON_PNG}")
    find_program(SIPS sips)
    find_program(ICONUTIL iconutil)
    if(SIPS AND ICONUTIL)
        set(_iconset "${CMAKE_BINARY_DIR}/nasrbrowse.iconset")
        add_custom_command(
            OUTPUT "${INSTALLER_ICON_ICNS}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${_iconset}"
            COMMAND ${SIPS} -z 16   16   "${ICON_PNG}" --out "${_iconset}/icon_16x16.png"       > /dev/null
            COMMAND ${SIPS} -z 32   32   "${ICON_PNG}" --out "${_iconset}/icon_16x16@2x.png"    > /dev/null
            COMMAND ${SIPS} -z 32   32   "${ICON_PNG}" --out "${_iconset}/icon_32x32.png"       > /dev/null
            COMMAND ${SIPS} -z 64   64   "${ICON_PNG}" --out "${_iconset}/icon_32x32@2x.png"    > /dev/null
            COMMAND ${SIPS} -z 128  128  "${ICON_PNG}" --out "${_iconset}/icon_128x128.png"     > /dev/null
            COMMAND ${SIPS} -z 256  256  "${ICON_PNG}" --out "${_iconset}/icon_128x128@2x.png"  > /dev/null
            COMMAND ${SIPS} -z 256  256  "${ICON_PNG}" --out "${_iconset}/icon_256x256.png"     > /dev/null
            COMMAND ${SIPS} -z 512  512  "${ICON_PNG}" --out "${_iconset}/icon_256x256@2x.png"  > /dev/null
            COMMAND ${SIPS} -z 512  512  "${ICON_PNG}" --out "${_iconset}/icon_512x512.png"     > /dev/null
            COMMAND ${SIPS} -z 1024 1024 "${ICON_PNG}" --out "${_iconset}/icon_512x512@2x.png"  > /dev/null
            COMMAND ${ICONUTIL} -c icns -o "${INSTALLER_ICON_ICNS}" "${_iconset}"
            DEPENDS "${ICON_PNG}"
            COMMENT "Generating ${INSTALLER_ICON_ICNS} from nasrbrowse.png"
            VERBATIM
        )
        add_custom_target(app_icon DEPENDS "${INSTALLER_ICON_ICNS}")
        add_dependencies(nasrbrowse app_icon)
    endif()
endif()

if(WIN32 AND EXISTS "${ICON_PNG}")
    find_program(MAGICK magick)
    if(MAGICK)
        add_custom_command(
            OUTPUT "${INSTALLER_ICON_ICO}"
            COMMAND ${MAGICK} "${ICON_PNG}" -define icon:auto-resize=16,32,48,64,128,256 "${INSTALLER_ICON_ICO}"
            DEPENDS "${ICON_PNG}"
            COMMENT "Generating ${INSTALLER_ICON_ICO} from nasrbrowse.png"
            VERBATIM
        )
        add_custom_target(app_icon DEPENDS "${INSTALLER_ICON_ICO}")
        add_dependencies(nasrbrowse app_icon)
    endif()
endif()

function(_require_asset path)
    if(NOT EXISTS "${path}")
        message(FATAL_ERROR
            "Installer asset missing: ${path}\n"
            "Generate prebuilt assets (see README 'Data Preparation') before running cpack.")
    endif()
endfunction()

if(APPLE)
    # =========================================================================
    # macOS: bundle + DragNDrop DMG
    # =========================================================================
    set_target_properties(nasrbrowse PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "NASRBrowse"
        MACOSX_BUNDLE_BUNDLE_VERSION "${APPLICATION_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${APPLICATION_VERSION}"
        MACOSX_BUNDLE_GUI_IDENTIFIER "${BUNDLE_IDENTIFIER}"
        MACOSX_BUNDLE_COPYRIGHT "existens.org"
    )
    if(TARGET app_icon)
        set_target_properties(nasrbrowse PROPERTIES
            MACOSX_BUNDLE_ICON_FILE "nasrbrowse.icns")
        target_sources(nasrbrowse PRIVATE "${INSTALLER_ICON_ICNS}")
        set_source_files_properties("${INSTALLER_ICON_ICNS}" PROPERTIES
            MACOSX_PACKAGE_LOCATION Resources
            GENERATED TRUE)
    endif()

    install(TARGETS nasrbrowse BUNDLE DESTINATION .)

    # Bundled assets go into Contents/Resources/ alongside the icon
    set(_rsrc "nasrbrowse.app/Contents/Resources")
    install(FILES "${BUNDLED_DB}"  DESTINATION "${_rsrc}")
    install(FILES "${BUNDLED_INI}" DESTINATION "${_rsrc}")
    install(DIRECTORY "${BUNDLED_BASEMAP}" DESTINATION "${_rsrc}")

    # Bundle brew-installed SDL dylibs into the .app via fixup_bundle at
    # install time. Runs inside the install prefix so paths resolve correctly.
    install(CODE "
        include(BundleUtilities)
        set(_bundle \"\${CMAKE_INSTALL_PREFIX}/nasrbrowse.app\")
        fixup_bundle(\"\${_bundle}\" \"\" \"/usr/local/lib;/opt/homebrew/lib\")
    " COMPONENT applications)

    set(CPACK_GENERATOR "DragNDrop")
    set(CPACK_PACKAGE_NAME "NASRBrowse")
    set(CPACK_PACKAGE_VENDOR "existens.org")
    set(CPACK_PACKAGE_VERSION_MAJOR "${MAJOR_VERSION}")
    set(CPACK_PACKAGE_VERSION_MINOR "${MINOR_VERSION}")
    set(CPACK_PACKAGE_VERSION_PATCH "${BUILD_VERSION}")
    set(CPACK_DMG_VOLUME_NAME "NASRBrowse ${APPLICATION_VERSION}")
    set(CPACK_DMG_FORMAT "UDZO")
    if(EXISTS "${CMAKE_SOURCE_DIR}/LICENSE")
        set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/LICENSE")
    endif()

    _require_asset("${BUNDLED_DB}")
    _require_asset("${BUNDLED_BASEMAP}")
    _require_asset("${BUNDLED_INI}")
    include(CPack)

elseif(WIN32)
    # =========================================================================
    # Windows: flat install tree + NSIS installer
    # =========================================================================
    install(TARGETS nasrbrowse RUNTIME DESTINATION bin)
    install(FILES "${BUNDLED_DB}"  DESTINATION bin)
    install(FILES "${BUNDLED_INI}" DESTINATION bin)
    install(DIRECTORY "${BUNDLED_BASEMAP}" DESTINATION bin)

    # MinGW runtime DLLs (SDL3/SDL3_image/SDL3_ttf/sqlite3 are statically
    # linked by build-mingw-deps.sh, so only libstdc++/libgcc/winpthread
    # need to ship).
    set(_mingw_runtime_dlls
        libgcc_s_seh-1.dll
        libstdc++-6.dll
        libwinpthread-1.dll
    )
    foreach(_dll ${_mingw_runtime_dlls})
        execute_process(
            COMMAND ${CMAKE_CXX_COMPILER} -print-file-name=${_dll}
            OUTPUT_VARIABLE _dll_path
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )
        if(_dll_path AND NOT _dll_path STREQUAL "${_dll}" AND EXISTS "${_dll_path}")
            install(FILES "${_dll_path}" DESTINATION bin)
        endif()
    endforeach()

    set(CPACK_GENERATOR "NSIS")
    set(CPACK_PACKAGE_NAME "NASRBrowse")
    set(CPACK_PACKAGE_VENDOR "existens.org")
    set(CPACK_PACKAGE_VERSION_MAJOR "${MAJOR_VERSION}")
    set(CPACK_PACKAGE_VERSION_MINOR "${MINOR_VERSION}")
    set(CPACK_PACKAGE_VERSION_PATCH "${BUILD_VERSION}")
    set(CPACK_PACKAGE_INSTALL_DIRECTORY "NASRBrowse")
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "FAA NASR chart viewer")
    if(EXISTS "${CMAKE_SOURCE_DIR}/LICENSE")
        set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/LICENSE")
    endif()

    set(CPACK_NSIS_DISPLAY_NAME "NASRBrowse")
    set(CPACK_NSIS_PACKAGE_NAME "NASRBrowse")
    set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
    set(CPACK_NSIS_MODIFY_PATH OFF)
    set(CPACK_NSIS_INSTALLED_ICON_NAME "bin\\\\nasrbrowse.exe")
    if(TARGET app_icon)
        set(CPACK_NSIS_MUI_ICON    "${INSTALLER_ICON_ICO}")
        set(CPACK_NSIS_MUI_UNIICON "${INSTALLER_ICON_ICO}")
    endif()

    # Start menu shortcut
    set(CPACK_NSIS_CREATE_ICONS_EXTRA
        "CreateShortCut '$SMPROGRAMS\\\\$STARTMENU_FOLDER\\\\NASRBrowse.lnk' '$INSTDIR\\\\bin\\\\nasrbrowse.exe'")
    set(CPACK_NSIS_DELETE_ICONS_EXTRA
        "Delete '$SMPROGRAMS\\\\$STARTMENU_FOLDER\\\\NASRBrowse.lnk'")

    _require_asset("${BUNDLED_DB}")
    _require_asset("${BUNDLED_BASEMAP}")
    _require_asset("${BUNDLED_INI}")
    include(CPack)
endif()
