#pragma once

#include "opaque_typedef.hpp"
#include <cstdint>
#include <glm/glm.hpp>

/**
 * SDL GPU type wrappers and forward declarations.
 *
 * This header provides C++ wrapper types for SDL GPU enums and forward
 * declarations for SDL GPU pointer types, allowing SDL wrapper headers
 * to avoid including SDL headers directly.
 *
 * Enum values are defined in types.cpp using actual SDL constants,
 * eliminating the need to keep magic numbers in sync.
 */

// ============================================================================
// Forward declarations for SDL GPU pointer types (in global namespace)
// ============================================================================

struct SDL_GPUBuffer;
struct SDL_GPUCommandBuffer;
struct SDL_GPUCopyPass;
struct SDL_GPUDevice;
struct SDL_GPURenderPass;
struct SDL_GPUTexture;
struct SDL_GPUGraphicsPipeline;
struct SDL_GPUSampler;
struct SDL_GPUShader;
struct SDL_GPUTransferBuffer;
struct SDL_GPUTextureCreateInfo;
struct SDL_Surface;
struct SDL_Rect;
struct SDL_Window;

// ============================================================================
// Forward declarations for SDL_ttf types (in global namespace)
// ============================================================================

struct TTF_Font;
struct TTF_TextEngine;
struct TTF_Text;

namespace sdl
{
    // ========================================================================
    // GPU Buffer Usage Flags (opaque typedef)
    // ========================================================================

    opaque_typedef(uint32_t, buffer_usage_t);

    namespace buffer_usage
    {
        extern const buffer_usage_t vertex;
        extern const buffer_usage_t index;
        extern const buffer_usage_t indirect;
        extern const buffer_usage_t graphics_storage_read;
        extern const buffer_usage_t compute_storage_read;
        extern const buffer_usage_t compute_storage_write;
    }

    // ========================================================================
    // GPU Texture Filtering (opaque typedef)
    // ========================================================================

    opaque_typedef(uint32_t, filter_t);

    namespace filter
    {
        extern const filter_t nearest;
        extern const filter_t linear;
    }

    // ========================================================================
    // GPU Sampler Address Mode (opaque typedef)
    // ========================================================================

    opaque_typedef(uint32_t, sampler_address_mode_t);

    namespace sampler_address_mode
    {
        extern const sampler_address_mode_t repeat;
        extern const sampler_address_mode_t mirrored_repeat;
        extern const sampler_address_mode_t clamp_to_edge;
    }

    // ========================================================================
    // GPU Shader Stage (opaque typedef)
    // ========================================================================

    opaque_typedef(uint32_t, shader_stage_t);

    namespace shader_stage
    {
        extern const shader_stage_t vertex;
        extern const shader_stage_t fragment;
    }

    // ========================================================================
    // GPU Shader Format (opaque typedef)
    // ========================================================================

    opaque_typedef(uint32_t, shader_format_t);

    namespace shader_format
    {
        extern const shader_format_t invalid;
        extern const shader_format_t private_;
        extern const shader_format_t spirv;
        extern const shader_format_t dxbc;
        extern const shader_format_t dxil;
        extern const shader_format_t msl;
        extern const shader_format_t metallib;
    }

    // ========================================================================
    // GPU Primitive Type (opaque typedef)
    // ========================================================================

    opaque_typedef(uint32_t, primitive_type_t);

    namespace primitive_type
    {
        extern const primitive_type_t triangle_list;
        extern const primitive_type_t triangle_strip;
        extern const primitive_type_t line_list;
        extern const primitive_type_t line_strip;
        extern const primitive_type_t point_list;
    }

    // ========================================================================
    // GPU Texture Format (opaque typedef)
    // ========================================================================

    opaque_typedef(uint32_t, texture_format_t);

    namespace texture_format
    {
        extern const texture_format_t d16_unorm;
        extern const texture_format_t d24_unorm;
        extern const texture_format_t d32_float;
        extern const texture_format_t d24_unorm_s8_uint;
        extern const texture_format_t d32_float_s8_uint;
    }

    // ========================================================================
    // Window Flags (opaque typedef)
    // ========================================================================

    opaque_typedef(uint64_t, window_flags_t);

    namespace window_flags
    {
        extern const window_flags_t resizable;
        extern const window_flags_t borderless;
        extern const window_flags_t fullscreen;
        extern const window_flags_t high_pixel_density;
        extern const window_flags_t hidden;
    }

    // ========================================================================
    // Audio Format (opaque typedef)
    // ========================================================================

    opaque_typedef(uint16_t, audio_format_t);

    namespace audio_format
    {
        extern const audio_format_t u8;  // Unsigned 8-bit
        extern const audio_format_t s8;  // Signed 8-bit
        extern const audio_format_t s16; // Signed 16-bit (native endian)
        extern const audio_format_t s32; // Signed 32-bit (native endian)
        extern const audio_format_t f32; // 32-bit floating point
    }

    // ========================================================================
    // Input Types (opaque typedef)
    // ========================================================================

    opaque_typedef(int32_t, input_key_t);
    opaque_typedef(uint8_t, input_button_t);
    opaque_typedef(int, input_action_t);
    opaque_typedef(uint16_t, input_mod_t);

    namespace input_action
    {
        extern const input_action_t release;
        extern const input_action_t press;
        extern const input_action_t repeat;
    }

    namespace input_mod
    {
        extern const input_mod_t shift;
        extern const input_mod_t control;
        extern const input_mod_t alt;
        extern const input_mod_t super;
    }

    // ========================================================================
    // Vertex and Uniform Buffer Structures
    // ========================================================================
    struct vertex_t2f_c4ub_v3f
    {
        float s, t;         // Texture coordinates (8 bytes)
        uint8_t r, g, b, a; // Color (4 bytes)
        float x, y, z;      // Position (12 bytes)
    };

    // ============================================================================
    // Uniform Buffer Structures
    // ============================================================================

    // Universal uniform buffer structure (superset of all shader uniforms)
    // All shaders accept this structure but only use the fields they need.
    // This eliminates the need for multiple uniform structures and simplifies
    // shader switching without changing uniform layout.
    // Total size: 352 bytes (naturally aligned)
    struct uniform_buffer
    {
        // Core transformation matrices (used by ALL shaders)
        glm::mat4 projection_matrix; // 64 bytes, offset 0   - Camera projection
        glm::mat4 view_matrix;       // 64 bytes, offset 64  - Camera view transform
        glm::mat4 model_matrix;      // 64 bytes, offset 128 - Model-to-world transform
        glm::mat4 texture_matrix;    // 64 bytes, offset 192 - Texture coordinate transform
        glm::mat4 color_matrix;      // 64 bytes, offset 256 - Color transformation

        // Text outline parameters (used by outline_text shader)
        glm::vec4 outline_color; // 16 bytes, offset 320 - RGBA outline color
        glm::ivec2 texture_size; // 8 bytes, offset 336  - Texture atlas dimensions

        // Y-cut parameters (used by ALL shaders)
        float y_min; // 4 bytes, offset 344 - Minimum Y for clip test
        float y_max; // 4 bytes, offset 348 - Maximum Y for clip test

        // Default constructor: Initialize all fields to safe defaults
        uniform_buffer() : projection_matrix(1.0F), // Identity matrix
                           view_matrix(1.0F),       // Identity matrix
                           model_matrix(1.0F),      // Identity matrix
                           texture_matrix(1.0F),    // Identity matrix
                           color_matrix(1.0F),      // Identity matrix
                           outline_color(0.0F),     // Transparent black
                           texture_size(0),         // Zero size
                           y_min(-1e9F),            // Default min far below (effectively disabled)
                           y_max(1e9F)              // Default max far above (effectively disabled)
        {
        }
    };
} // namespace sdl
