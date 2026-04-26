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
    # macOS: Metal shader pipeline.
    # The metal/metallib compilers ship only with full Xcode (the standalone
    # "Metal Developer Tools for macOS" download is no longer published as of
    # 2026; Apple's developer page redirects to "install Xcode"). When they're
    # available we precompile .metallib bytecode for fastest startup. When
    # they're not, we ship MSL source and let the Metal driver compile it on
    # first use — Command Line Tools alone is enough for that path.
    find_program(XCRUN xcrun)
    if(NOT XCRUN)
        message(FATAL_ERROR "xcrun not found - macOS shaders require at least Xcode Command Line Tools (xcode-select --install)")
    endif()

    if(OSECT_ENABLE_METALLIB)
        execute_process(
            COMMAND ${XCRUN} -f metal
            OUTPUT_VARIABLE METAL_PATH
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
            RESULT_VARIABLE METAL_RESULT
        )
        if(METAL_RESULT EQUAL 0)
            set(OSECT_BUILD_METALLIB TRUE)
            message(STATUS "Metal: precompiled .metallib pipeline (metal compiler at ${METAL_PATH})")
        else()
            set(OSECT_BUILD_METALLIB FALSE)
            message(STATUS "Metal: MSL-source pipeline (metal compiler not found; install full Xcode for precompiled .metallib startup)")
        endif()
    else()
        set(OSECT_BUILD_METALLIB FALSE)
        message(STATUS "Metal: MSL-source pipeline (OSECT_ENABLE_METALLIB=OFF)")
    endif()
    set(OSECT_BUILD_METALLIB ${OSECT_BUILD_METALLIB} PARENT_SCOPE)
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
        "  Debian/Ubuntu: apt install glslang-tools\n"
        "  Alpine:        apk add glslang\n"
        "  macOS:         brew install glslang   (or 'sudo port install glslang')\n"
        "  Windows:       install the Vulkan SDK and source its setup script")
endif()

# D3D12 backend is experimental and opt-in. Built only on Windows when both the
# OSECT_ENABLE_D3D12 option is ON and dxc is available (DXIL has no other producer).
# Without DXIL the binary still runs on Windows via the Vulkan backend.
if(WIN32 AND OSECT_ENABLE_D3D12 AND DXC)
    set(OSECT_BUILD_D3D12 TRUE)
    message(STATUS "D3D12 backend: enabled (dxc=${DXC})")
elseif(WIN32 AND OSECT_ENABLE_D3D12)
    set(OSECT_BUILD_D3D12 FALSE)
    message(STATUS "D3D12 backend: disabled (OSECT_ENABLE_D3D12=ON but dxc not found)")
else()
    set(OSECT_BUILD_D3D12 FALSE)
endif()
set(OSECT_BUILD_D3D12 ${OSECT_BUILD_D3D12} PARENT_SCOPE)

if(APPLE)
    find_program(SPIRV_CROSS spirv-cross HINTS "${VULKAN_SDK_BIN}")
    if(NOT SPIRV_CROSS)
        message(FATAL_ERROR "spirv-cross not found - required for Metal shader compilation. Install via 'brew install spirv-cross', 'sudo port install spirv-cross', or the Vulkan SDK.")
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
        # -s = silent (suppress glslang's default echo of input filename;
        # errors still go to stderr, and our COMMENT line gives a better one).
        set(cmd ${GLSLANG} -s -V -D -S ${gstage} -e ${entry_point} --hlsl-iomap ${defines} ${hlsl_source} -o ${output_spv})
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
        # macOS pipeline outputs (always):
        #   HLSL → SPIR-V → .h           (MoltenVK fallback)
        #   HLSL → SPIR-V → MSL → .h     (driver compiles MSL at runtime)
        # Additionally if the metal compiler is available:
        #   MSL → .air → .metallib → .h  (precompiled, fastest startup)
        set(SHADER_HEADERS "")
        set(AIR_FILES "")

        foreach(STAGE vertex fragment)
            if(STAGE STREQUAL "vertex")
                set(ENTRY_POINT "vertex_main")
                set(STAGE_SUFFIX "vert")
            else()
                set(ENTRY_POINT "fragment_main")
                set(STAGE_SUFFIX "frag")
            endif()

            set(STAGE_SPV   "${CMAKE_CURRENT_BINARY_DIR}/${SHADER}_${STAGE_SUFFIX}.spv")
            set(STAGE_METAL "${CMAKE_CURRENT_BINARY_DIR}/${SHADER}_${STAGE_SUFFIX}.metal")
            set(HEADER_STAGE_SPV "${CMAKE_CURRENT_BINARY_DIR}/${SHADER}_${STAGE_SUFFIX}_spv.h")
            set(HEADER_STAGE_MSL "${CMAKE_CURRENT_BINARY_DIR}/${SHADER}_${STAGE_SUFFIX}_metal.h")

            # HLSL → SPIR-V
            # SDL3 GPU descriptor sets: vertex uniforms=set1, fragment uniforms=set3, fragment samplers=set2
            if(STAGE STREQUAL "vertex")
                add_hlsl_spirv_command(${STAGE_SPV} ${HLSL_SOURCE} vertex ${ENTRY_POINT} -DVERTEX_SHADER)
            else()
                add_hlsl_spirv_command(${STAGE_SPV} ${HLSL_SOURCE} fragment ${ENTRY_POINT})
            endif()

            # SPIR-V → MSL source
            add_custom_command(
                OUTPUT ${STAGE_METAL}
                COMMAND ${SPIRV_CROSS} --msl ${STAGE_SPV} --output ${STAGE_METAL}
                DEPENDS ${STAGE_SPV}
                COMMENT "SPIR-V → MSL: ${SHADER}_${STAGE_SUFFIX}.spv → ${SHADER}_${STAGE_SUFFIX}.metal"
                VERBATIM
            )

            # Embed SPIR-V
            add_custom_command(
                OUTPUT ${HEADER_STAGE_SPV}
                COMMAND ${XXD} -i ${SHADER}_${STAGE_SUFFIX}.spv > ${HEADER_STAGE_SPV}
                WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
                DEPENDS ${STAGE_SPV}
                COMMENT "Embedding SPIR-V: ${SHADER}_${STAGE_SUFFIX}.spv → ${SHADER}_${STAGE_SUFFIX}_spv.h"
                VERBATIM
            )

            # Embed MSL source
            add_custom_command(
                OUTPUT ${HEADER_STAGE_MSL}
                COMMAND ${XXD} -i ${SHADER}_${STAGE_SUFFIX}.metal > ${HEADER_STAGE_MSL}
                WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
                DEPENDS ${STAGE_METAL}
                COMMENT "Embedding MSL source: ${SHADER}_${STAGE_SUFFIX}.metal → ${SHADER}_${STAGE_SUFFIX}_msl.h"
                VERBATIM
            )

            list(APPEND SHADER_HEADERS ${HEADER_STAGE_SPV} ${HEADER_STAGE_MSL})

            if(OSECT_BUILD_METALLIB)
                set(STAGE_AIR "${CMAKE_CURRENT_BINARY_DIR}/${SHADER}_${STAGE_SUFFIX}.air")
                add_custom_command(
                    OUTPUT ${STAGE_AIR}
                    COMMAND ${XCRUN} metal -c ${STAGE_METAL} -o ${STAGE_AIR}
                    DEPENDS ${STAGE_METAL}
                    COMMENT "MSL → AIR: ${SHADER}_${STAGE_SUFFIX}.metal → ${SHADER}_${STAGE_SUFFIX}.air"
                    VERBATIM
                )
                list(APPEND AIR_FILES ${STAGE_AIR})
            endif()
        endforeach()

        if(OSECT_BUILD_METALLIB)
            set(METALLIB_FILE "${CMAKE_CURRENT_BINARY_DIR}/${SHADER}.metallib")
            set(HEADER_METALLIB "${CMAKE_CURRENT_BINARY_DIR}/${SHADER}_metallib.h")
            add_custom_command(
                OUTPUT ${METALLIB_FILE}
                COMMAND ${XCRUN} metallib ${AIR_FILES} -o ${METALLIB_FILE}
                DEPENDS ${AIR_FILES}
                COMMENT "Linking Metal library: ${SHADER}.metallib"
                VERBATIM
            )
            add_custom_command(
                OUTPUT ${HEADER_METALLIB}
                COMMAND ${XXD} -i ${SHADER}.metallib > ${HEADER_METALLIB}
                WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
                DEPENDS ${METALLIB_FILE}
                COMMENT "Embedding metallib: ${SHADER}.metallib → ${SHADER}_metallib.h"
                VERBATIM
            )
            list(APPEND SHADER_HEADERS ${HEADER_METALLIB})
        endif()

        add_custom_target(${SHADER}_shader DEPENDS ${SHADER_HEADERS})
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

        # Optionally compile HLSL → DXIL for the D3D12 backend (Windows only, opt-in).
        if(OSECT_BUILD_D3D12)
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
