cmake_minimum_required(VERSION 3.10)
project(nasrbrowse CXX)

# Export compile commands
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Find SDL3 (system-installed)
find_package(SDL3 REQUIRED CONFIG)
find_package(SDL3_image REQUIRED CONFIG)
find_package(SDL3_ttf REQUIRED CONFIG)

# Main executable
add_executable(nasrbrowse
    # Application sources
    src/color.cpp
    src/layer.cpp
    src/layer_map.cpp
    src/main.cpp
    src/render_context.cpp

    # SDL wrapper sources
    sdl/buffer.cpp
    sdl/command_buffer.cpp
    sdl/copy_pass.cpp
    sdl/depth_buffer.cpp
    sdl/device.cpp
    sdl/error.cpp
    sdl/event.cpp
    sdl/font.cpp
    sdl/instance.cpp
    sdl/pipeline.cpp
    sdl/render_pass.cpp
    sdl/sampler.cpp
    sdl/shader.cpp
    sdl/surface.cpp
    sdl/text.cpp
    sdl/text_engine.cpp
    sdl/texture.cpp
    sdl/transfer_buffer.cpp
    sdl/types.cpp
    sdl/window.cpp
)

# Include directories
target_include_directories(nasrbrowse PRIVATE
    ${CMAKE_SOURCE_DIR}        # For #include <sdl/...>
    ${CMAKE_SOURCE_DIR}/src    # For #include "layer.hpp" etc.
    thirdparty/glm-1.0.1
)

# C++ standard
target_compile_features(nasrbrowse PRIVATE cxx_std_17)

# Compiler warnings (all build types)
target_compile_options(nasrbrowse PRIVATE -Wall -Wextra)

# Build-type-specific compiler options
target_compile_options(nasrbrowse PRIVATE
    $<$<CONFIG:Debug>:-O0 -ggdb -fsanitize=address>
    $<$<CONFIG:Release>:-O3>
    $<$<CONFIG:RelWithDebInfo>:-O2 -g -pg>
)

# Build-type-specific definitions
target_compile_definitions(nasrbrowse PRIVATE
    $<$<CONFIG:Debug>:DEBUG=1>
)

# Build-type-specific linker options
target_link_options(nasrbrowse PRIVATE
    $<$<CONFIG:Debug>:-fsanitize=address>
    $<$<CONFIG:RelWithDebInfo>:-pg>
)

# Link libraries
target_link_libraries(nasrbrowse PRIVATE
    SDL3::SDL3
    SDL3_image::SDL3_image
    SDL3_ttf::SDL3_ttf
)

# Shader compilation (Metal, SPIRV)
add_subdirectory(shaders)

# Add shader headers to include path
target_include_directories(nasrbrowse PRIVATE ${SHADER_INCLUDE_DIR})

# Ensure shaders are built before the executable
add_dependencies(nasrbrowse shaders)
