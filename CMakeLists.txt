cmake_minimum_required(VERSION 3.10)
project(nasrbrowse CXX)

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

# Main executable
add_executable(nasrbrowse
    # Application sources
    src/ini_config.cpp
    src/layer.cpp
    src/layer_map.cpp
    src/feature_builder.cpp
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
