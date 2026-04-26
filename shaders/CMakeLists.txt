# Shader compilation for SDL GPU
# Supports HLSL → SPIR-V → MSL → .metallib pipeline (macOS)
# Supports HLSL → SPIR-V pipeline (Linux)

# List of shaders to compile
set(SHADERS
    default
    line
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

# VULKAN_SDK is an optional hint for finding shader tools.
# When set (typical on macOS/Windows), its bin/ directory is searched first.
# When unset (e.g. Alpine), tools are found via the system PATH.
if(DEFINED ENV{VULKAN_SDK})
    set(VULKAN_SDK_PATH "$ENV{VULKAN_SDK}" CACHE PATH "Path to Vulkan SDK")
    set(VULKAN_SDK_BIN "${VULKAN_SDK_PATH}/bin")
else()
    set(VULKAN_SDK_BIN "")
endif()

find_program(XXD xxd)
if(NOT XXD)
    message(FATAL_ERROR "xxd not found - required for embedding shader bytecode")
endif()

# Find HLSL → SPIR-V compilers. Either works; glslang preferred if both present.
find_program(GLSLANG glslangValidator HINTS "${VULKAN_SDK_BIN}")
find_program(DXC dxc HINTS "${VULKAN_SDK_BIN}")

if(GLSLANG)
    set(SPIRV_COMPILER "glslang")
    message(STATUS "HLSL → SPIR-V: ${GLSLANG}")
elseif(DXC)
    set(SPIRV_COMPILER "dxc")
    message(STATUS "HLSL → SPIR-V: ${DXC} (glslangValidator not found)")
else()
    message(FATAL_ERROR
        "Neither glslangValidator nor dxc found - one is required for HLSL → SPIR-V compilation.\n"
        "  Alpine:        apk add glslang\n"
        "  Debian/Ubuntu: apt install glslang-tools\n"
        "  macOS/Windows: install the Vulkan SDK and source its setup script")
endif()

# Windows: DXIL pipeline for D3D12 backend requires dxc.
if(WIN32 AND NOT DXC)
    message(FATAL_ERROR "dxc not found - required for HLSL → DXIL compilation on Windows")
endif()

if(APPLE)
    find_program(SPIRV_CROSS spirv-cross HINTS "${VULKAN_SDK_BIN}")
    if(NOT SPIRV_CROSS)
        message(FATAL_ERROR "spirv-cross not found - required for Metal shader compilation (install Vulkan SDK or 'brew install spirv-cross')")
    endif()
endif()

# Helper: emit a HLSL → SPIR-V custom command using whichever compiler was selected.
# --hlsl-iomap (glslang) and the default DXC -spirv mapping both honor
# register(xN, spaceM) → (set=M, binding=N), which SDL3 GPU expects.
function(add_hlsl_spirv_command output_spv hlsl_source stage entry_point)
    set(defines ${ARGN})
    get_filename_component(src_name ${hlsl_source} NAME)
    get_filename_component(out_name ${output_spv} NAME)
    if(SPIRV_COMPILER STREQUAL "glslang")
        if(stage STREQUAL "vertex")
            set(gstage vert)
        else()
            set(gstage frag)
        endif()
        set(cmd ${GLSLANG} -V -D -S ${gstage} -e ${entry_point} --hlsl-iomap ${defines} ${hlsl_source} -o ${output_spv})
    else()
        if(stage STREQUAL "vertex")
            set(profile vs_6_0)
        else()
            set(profile ps_6_0)
        endif()
        set(cmd ${DXC} -spirv -T ${profile} -E ${entry_point} ${defines} ${hlsl_source} -Fo ${output_spv})
    endif()
    add_custom_command(
        OUTPUT ${output_spv}
        COMMAND ${cmd}
        DEPENDS ${hlsl_source}
        COMMENT "Compiling HLSL ${stage} shader (${SPIRV_COMPILER}): ${src_name} → ${out_name}"
        VERBATIM
    )
endfunction()

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
        # macOS: HLSL → SPIR-V → MSL → .metallib → .h (Metal path)
        #        HLSL → SPIR-V → .h (Vulkan/MoltenVK path)
        set(METALLIB_FILE "${CMAKE_CURRENT_BINARY_DIR}/${SHADER}.metallib")
        set(HEADER_FILE "${CMAKE_CURRENT_BINARY_DIR}/${SHADER}_metallib.h")
        set(HEADER_VERT_SPV "${CMAKE_CURRENT_BINARY_DIR}/${SHADER}_vert_spv.h")
        set(HEADER_FRAG_SPV "${CMAKE_CURRENT_BINARY_DIR}/${SHADER}_frag_spv.h")
        set(AIR_FILES "")

        # Process both vertex and fragment stages
        foreach(STAGE vertex fragment)
            # Stage-specific parameters
            if(STAGE STREQUAL "vertex")
                set(ENTRY_POINT "vertex_main")
                set(STAGE_SUFFIX "vert")
            else()
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
                add_hlsl_spirv_command(${STAGE_SPV} ${HLSL_SOURCE} vertex ${ENTRY_POINT} -DVERTEX_SHADER)
            else()
                add_hlsl_spirv_command(${STAGE_SPV} ${HLSL_SOURCE} fragment ${ENTRY_POINT})
            endif()

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

        # Embed SPIR-V → .h (for Vulkan/MoltenVK path)
        add_custom_command(
            OUTPUT ${HEADER_VERT_SPV}
            COMMAND ${XXD} -i ${SHADER}_vert.spv > ${HEADER_VERT_SPV}
            WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
            DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/${SHADER}_vert.spv
            COMMENT "Embedding vertex shader bytecode: ${SHADER}_vert.spv → ${SHADER}_vert_spv.h"
            VERBATIM
        )

        add_custom_command(
            OUTPUT ${HEADER_FRAG_SPV}
            COMMAND ${XXD} -i ${SHADER}_frag.spv > ${HEADER_FRAG_SPV}
            WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
            DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/${SHADER}_frag.spv
            COMMENT "Embedding fragment shader bytecode: ${SHADER}_frag.spv → ${SHADER}_frag_spv.h"
            VERBATIM
        )

        # Add to shader target dependencies
        add_custom_target(${SHADER}_shader DEPENDS ${HEADER_FILE} ${HEADER_VERT_SPV} ${HEADER_FRAG_SPV})
        add_dependencies(shaders ${SHADER}_shader)

    else()
        # Non-Apple: HLSL → SPIR-V (always) + HLSL → DXIL (Windows only)
        set(SPV_VERT "${CMAKE_CURRENT_BINARY_DIR}/${SHADER}_vert.spv")
        set(SPV_FRAG "${CMAKE_CURRENT_BINARY_DIR}/${SHADER}_frag.spv")
        set(HEADER_VERT "${CMAKE_CURRENT_BINARY_DIR}/${SHADER}_vert_spv.h")
        set(HEADER_FRAG "${CMAKE_CURRENT_BINARY_DIR}/${SHADER}_frag_spv.h")

        # HLSL → SPIR-V
        # SDL3 GPU descriptor sets: vertex uniforms=set1, fragment uniforms=set3, fragment samplers=set2
        add_hlsl_spirv_command(${SPV_VERT} ${HLSL_SOURCE} vertex vertex_main -DVERTEX_SHADER)
        add_hlsl_spirv_command(${SPV_FRAG} ${HLSL_SOURCE} fragment fragment_main)

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

        set(SHADER_HEADERS ${HEADER_VERT} ${HEADER_FRAG})

        # Windows: also compile HLSL → DXIL for D3D12 backend
        if(WIN32)
            set(DXIL_VERT "${CMAKE_CURRENT_BINARY_DIR}/${SHADER}_vert.dxil")
            set(DXIL_FRAG "${CMAKE_CURRENT_BINARY_DIR}/${SHADER}_frag.dxil")
            set(HEADER_DXIL_VERT "${CMAKE_CURRENT_BINARY_DIR}/${SHADER}_vert_dxil.h")
            set(HEADER_DXIL_FRAG "${CMAKE_CURRENT_BINARY_DIR}/${SHADER}_frag_dxil.h")

            # HLSL → DXIL (vertex)
            add_custom_command(
                OUTPUT ${DXIL_VERT}
                COMMAND ${DXC} -T vs_6_0 -E vertex_main -DVERTEX_SHADER -DRAW_BUFFERS ${HLSL_SOURCE} -Fo ${DXIL_VERT}
                DEPENDS ${HLSL_SOURCE}
                COMMENT "Compiling HLSL vertex shader (DXIL): ${SHADER}.hlsl → ${SHADER}_vert.dxil"
                VERBATIM
            )

            # HLSL → DXIL (fragment)
            add_custom_command(
                OUTPUT ${DXIL_FRAG}
                COMMAND ${DXC} -T ps_6_0 -E fragment_main -DRAW_BUFFERS ${HLSL_SOURCE} -Fo ${DXIL_FRAG}
                DEPENDS ${HLSL_SOURCE}
                COMMENT "Compiling HLSL fragment shader (DXIL): ${SHADER}.hlsl → ${SHADER}_frag.dxil"
                VERBATIM
            )

            # Embed DXIL → .h
            add_custom_command(
                OUTPUT ${HEADER_DXIL_VERT}
                COMMAND ${XXD} -i ${SHADER}_vert.dxil > ${HEADER_DXIL_VERT}
                WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
                DEPENDS ${DXIL_VERT}
                COMMENT "Embedding vertex shader bytecode: ${SHADER}_vert.dxil → ${SHADER}_vert_dxil.h"
                VERBATIM
            )

            add_custom_command(
                OUTPUT ${HEADER_DXIL_FRAG}
                COMMAND ${XXD} -i ${SHADER}_frag.dxil > ${HEADER_DXIL_FRAG}
                WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
                DEPENDS ${DXIL_FRAG}
                COMMENT "Embedding fragment shader bytecode: ${SHADER}_frag.dxil → ${SHADER}_frag_dxil.h"
                VERBATIM
            )

            list(APPEND SHADER_HEADERS ${HEADER_DXIL_VERT} ${HEADER_DXIL_FRAG})
        endif()

        # Add to shader target dependencies
        add_custom_target(${SHADER}_shader DEPENDS ${SHADER_HEADERS})
        add_dependencies(shaders ${SHADER}_shader)
    endif()
endforeach()

# Make shader headers available to parent targets
set(SHADER_INCLUDE_DIR ${CMAKE_CURRENT_BINARY_DIR} PARENT_SCOPE)
