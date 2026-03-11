# Shader compilation for SDL GPU
# Supports HLSL → SPIR-V → MSL → .metallib pipeline (macOS)
# Supports HLSL → SPIR-V pipeline (Linux)

# List of shaders to compile
set(SHADERS
    default
    textured
)

# Find platform-specific tools
if(APPLE)
    # macOS: Metal shader pipeline
    find_program(XCRUN xcrun)
    if(NOT XCRUN)
        message(FATAL_ERROR "xcrun not found - Metal shaders require Xcode Command Line Tools")
    endif()

    # Verify Metal compiler is available (requires full Xcode, not just Command Line Tools)
    execute_process(
        COMMAND ${XCRUN} -f metal
        OUTPUT_VARIABLE METAL_PATH
        ERROR_QUIET
        RESULT_VARIABLE METAL_RESULT
    )
    if(NOT METAL_RESULT EQUAL 0)
        message(FATAL_ERROR
            "Metal compiler not found.\n"
            "The Metal compiler requires full Xcode (not just Command Line Tools).\n"
            "Install Xcode from the App Store, then run:\n"
            "  sudo xcode-select -s /Applications/Xcode.app\n"
            "  sudo xcodebuild -license accept")
    endif()
endif()

# Require VULKAN_SDK environment variable
if(NOT DEFINED ENV{VULKAN_SDK})
    message(FATAL_ERROR
        "VULKAN_SDK environment variable not set.\n"
        "Install the Vulkan SDK and source its setup script, or set VULKAN_SDK manually.")
endif()
set(VULKAN_SDK_PATH "$ENV{VULKAN_SDK}" CACHE PATH "Path to Vulkan SDK")

find_program(XXD xxd)
if(NOT XXD)
    message(FATAL_ERROR "xxd not found - required for embedding shader bytecode")
endif()

# Find DXC and SPIRV-Cross for HLSL pipeline
find_program(DXC dxc PATHS "${VULKAN_SDK_PATH}/bin" NO_DEFAULT_PATH)
find_program(SPIRV_CROSS spirv-cross PATHS "${VULKAN_SDK_PATH}/bin" NO_DEFAULT_PATH)

if(DXC AND SPIRV_CROSS)
    message(STATUS "HLSL shader pipeline available (DXC + SPIRV-Cross)")
else()
    message(FATAL_ERROR "HLSL shader pipeline not available - DXC and SPIRV-Cross required")
endif()

# Custom target for all shaders
add_custom_target(shaders ALL)

# Process each shader
foreach(SHADER ${SHADERS})
    set(HLSL_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/${SHADER}.hlsl")

    # Verify HLSL source exists
    if(NOT EXISTS ${HLSL_SOURCE})
        message(FATAL_ERROR "HLSL shader source not found: ${HLSL_SOURCE}")
    endif()

    if(APPLE)
        # macOS: HLSL → SPIR-V → MSL → .metallib → .h
        set(METALLIB_FILE "${CMAKE_CURRENT_BINARY_DIR}/${SHADER}.metallib")
        set(HEADER_FILE "${CMAKE_CURRENT_BINARY_DIR}/${SHADER}_metallib.h")
        set(AIR_FILES "")

        # Process both vertex and fragment stages
        foreach(STAGE vertex fragment)
            # Stage-specific parameters
            if(STAGE STREQUAL "vertex")
                set(SHADER_PROFILE "vs_6_0")
                set(ENTRY_POINT "vertex_main")
                set(STAGE_SUFFIX "vert")
            else()
                set(SHADER_PROFILE "ps_6_0")
                set(ENTRY_POINT "fragment_main")
                set(STAGE_SUFFIX "frag")
            endif()

            # File paths for this stage
            set(STAGE_SPV "${CMAKE_CURRENT_BINARY_DIR}/${SHADER}_${STAGE_SUFFIX}.spv")
            set(STAGE_METAL "${CMAKE_CURRENT_BINARY_DIR}/${SHADER}_${STAGE_SUFFIX}.metal")
            set(STAGE_AIR "${CMAKE_CURRENT_BINARY_DIR}/${SHADER}_${STAGE_SUFFIX}.air")

            # HLSL → SPIR-V
            # SDL3 GPU descriptor sets: vertex uniforms=set1, fragment uniforms=set3, fragment samplers=set2
            if(STAGE STREQUAL "vertex")
                set(STAGE_DEFINES -DVERTEX_SHADER)
            else()
                set(STAGE_DEFINES "")
            endif()
            add_custom_command(
                OUTPUT ${STAGE_SPV}
                COMMAND ${DXC} -spirv -T ${SHADER_PROFILE} -E ${ENTRY_POINT} ${STAGE_DEFINES} ${HLSL_SOURCE} -Fo ${STAGE_SPV}
                DEPENDS ${HLSL_SOURCE}
                COMMENT "Compiling HLSL ${STAGE} shader: ${SHADER}.hlsl → ${SHADER}_${STAGE_SUFFIX}.spv"
                VERBATIM
            )

            # SPIR-V → MSL
            add_custom_command(
                OUTPUT ${STAGE_METAL}
                COMMAND ${SPIRV_CROSS} --msl ${STAGE_SPV} --output ${STAGE_METAL}
                DEPENDS ${STAGE_SPV}
                COMMENT "Converting SPIR-V to MSL: ${SHADER}_${STAGE_SUFFIX}.spv → ${SHADER}_${STAGE_SUFFIX}.metal"
                VERBATIM
            )

            # MSL → .air
            add_custom_command(
                OUTPUT ${STAGE_AIR}
                COMMAND ${XCRUN} metal -c ${STAGE_METAL} -o ${STAGE_AIR}
                DEPENDS ${STAGE_METAL}
                COMMENT "Compiling Metal ${STAGE} shader: ${SHADER}_${STAGE_SUFFIX}.metal → ${SHADER}_${STAGE_SUFFIX}.air"
                VERBATIM
            )

            list(APPEND AIR_FILES ${STAGE_AIR})
        endforeach()

        # Link .air files → .metallib
        add_custom_command(
            OUTPUT ${METALLIB_FILE}
            COMMAND ${XCRUN} metallib ${AIR_FILES} -o ${METALLIB_FILE}
            DEPENDS ${AIR_FILES}
            COMMENT "Linking Metal library: ${SHADER}_vert.air + ${SHADER}_frag.air → ${SHADER}.metallib"
            VERBATIM
        )

        # Embed .metallib → .h
        add_custom_command(
            OUTPUT ${HEADER_FILE}
            COMMAND ${XXD} -i ${SHADER}.metallib > ${HEADER_FILE}
            WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
            DEPENDS ${METALLIB_FILE}
            COMMENT "Embedding shader bytecode: ${SHADER}.metallib → ${SHADER}_metallib.h"
            VERBATIM
        )

        # Add to shader target dependencies
        add_custom_target(${SHADER}_shader DEPENDS ${HEADER_FILE})
        add_dependencies(shaders ${SHADER}_shader)

    else()
        # Linux: HLSL → SPIR-V → .h
        set(SPV_VERT "${CMAKE_CURRENT_BINARY_DIR}/${SHADER}_vert.spv")
        set(SPV_FRAG "${CMAKE_CURRENT_BINARY_DIR}/${SHADER}_frag.spv")
        set(HEADER_VERT "${CMAKE_CURRENT_BINARY_DIR}/${SHADER}_vert_spv.h")
        set(HEADER_FRAG "${CMAKE_CURRENT_BINARY_DIR}/${SHADER}_frag_spv.h")

        # HLSL → SPIR-V (vertex)
        # SDL3 GPU descriptor sets: vertex uniforms=set1, fragment uniforms=set3, fragment samplers=set2
        add_custom_command(
            OUTPUT ${SPV_VERT}
            COMMAND ${DXC} -spirv -T vs_6_0 -E vertex_main -DVERTEX_SHADER ${HLSL_SOURCE} -Fo ${SPV_VERT}
            DEPENDS ${HLSL_SOURCE}
            COMMENT "Compiling HLSL vertex shader: ${SHADER}.hlsl → ${SHADER}_vert.spv"
            VERBATIM
        )

        # HLSL → SPIR-V (fragment)
        add_custom_command(
            OUTPUT ${SPV_FRAG}
            COMMAND ${DXC} -spirv -T ps_6_0 -E fragment_main ${HLSL_SOURCE} -Fo ${SPV_FRAG}
            DEPENDS ${HLSL_SOURCE}
            COMMENT "Compiling HLSL fragment shader: ${SHADER}.hlsl → ${SHADER}_frag.spv"
            VERBATIM
        )

        # Embed vertex SPIR-V → .h
        add_custom_command(
            OUTPUT ${HEADER_VERT}
            COMMAND ${XXD} -i ${SHADER}_vert.spv > ${HEADER_VERT}
            WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
            DEPENDS ${SPV_VERT}
            COMMENT "Embedding vertex shader bytecode: ${SHADER}_vert.spv → ${SHADER}_vert_spv.h"
            VERBATIM
        )

        # Embed fragment SPIR-V → .h
        add_custom_command(
            OUTPUT ${HEADER_FRAG}
            COMMAND ${XXD} -i ${SHADER}_frag.spv > ${HEADER_FRAG}
            WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
            DEPENDS ${SPV_FRAG}
            COMMENT "Embedding fragment shader bytecode: ${SHADER}_frag.spv → ${SHADER}_frag_spv.h"
            VERBATIM
        )

        # Add to shader target dependencies
        add_custom_target(${SHADER}_shader DEPENDS ${HEADER_VERT} ${HEADER_FRAG})
        add_dependencies(shaders ${SHADER}_shader)
    endif()
endforeach()

# Make shader headers available to parent targets
set(SHADER_INCLUDE_DIR ${CMAKE_CURRENT_BINARY_DIR} PARENT_SCOPE)
