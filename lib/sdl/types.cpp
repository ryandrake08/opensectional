#include "types.hpp"
#include <SDL3/SDL.h>

namespace sdl
{
    // ========================================================================
    // GPU Buffer Usage Flags
    // ========================================================================

    namespace buffer_usage
    {
        const buffer_usage_t vertex(SDL_GPU_BUFFERUSAGE_VERTEX);
        const buffer_usage_t index(SDL_GPU_BUFFERUSAGE_INDEX);
        const buffer_usage_t indirect(SDL_GPU_BUFFERUSAGE_INDIRECT);
        const buffer_usage_t graphics_storage_read(SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ);
        const buffer_usage_t compute_storage_read(SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ);
        const buffer_usage_t compute_storage_write(SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE);
    }

    // ========================================================================
    // GPU Texture Filtering
    // ========================================================================

    namespace filter
    {
        const filter_t nearest(SDL_GPU_FILTER_NEAREST);
        const filter_t linear(SDL_GPU_FILTER_LINEAR);
    }

    // ========================================================================
    // GPU Sampler Address Mode
    // ========================================================================

    namespace sampler_address_mode
    {
        const sampler_address_mode_t repeat(SDL_GPU_SAMPLERADDRESSMODE_REPEAT);
        const sampler_address_mode_t mirrored_repeat(SDL_GPU_SAMPLERADDRESSMODE_MIRRORED_REPEAT);
        const sampler_address_mode_t clamp_to_edge(SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE);
    }

    // ========================================================================
    // GPU Shader Stage
    // ========================================================================

    namespace shader_stage
    {
        const shader_stage_t vertex(SDL_GPU_SHADERSTAGE_VERTEX);
        const shader_stage_t fragment(SDL_GPU_SHADERSTAGE_FRAGMENT);
    }

    // ========================================================================
    // GPU Shader Format
    // ========================================================================

    namespace shader_format
    {
        const shader_format_t invalid(SDL_GPU_SHADERFORMAT_INVALID);
        const shader_format_t private_(SDL_GPU_SHADERFORMAT_PRIVATE);
        const shader_format_t spirv(SDL_GPU_SHADERFORMAT_SPIRV);
        const shader_format_t dxbc(SDL_GPU_SHADERFORMAT_DXBC);
        const shader_format_t dxil(SDL_GPU_SHADERFORMAT_DXIL);
        const shader_format_t msl(SDL_GPU_SHADERFORMAT_MSL);
        const shader_format_t metallib(SDL_GPU_SHADERFORMAT_METALLIB);
    }

    // ========================================================================
    // GPU Primitive Type
    // ========================================================================

    namespace primitive_type
    {
        const primitive_type_t triangle_list(SDL_GPU_PRIMITIVETYPE_TRIANGLELIST);
        const primitive_type_t triangle_strip(SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP);
        const primitive_type_t line_list(SDL_GPU_PRIMITIVETYPE_LINELIST);
        const primitive_type_t line_strip(SDL_GPU_PRIMITIVETYPE_LINESTRIP);
        const primitive_type_t point_list(SDL_GPU_PRIMITIVETYPE_POINTLIST);
    }

    // ========================================================================
    // GPU Texture Format (depth formats)
    // ========================================================================

    namespace texture_format
    {
        const texture_format_t d16_unorm(SDL_GPU_TEXTUREFORMAT_D16_UNORM);
        const texture_format_t d24_unorm(SDL_GPU_TEXTUREFORMAT_D24_UNORM);
        const texture_format_t d32_float(SDL_GPU_TEXTUREFORMAT_D32_FLOAT);
        const texture_format_t d24_unorm_s8_uint(SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT);
        const texture_format_t d32_float_s8_uint(SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT);
    }

    // ========================================================================
    // Window Flags
    // ========================================================================

    namespace window_flags
    {
        const window_flags_t resizable(SDL_WINDOW_RESIZABLE);
        const window_flags_t borderless(SDL_WINDOW_BORDERLESS);
        const window_flags_t fullscreen(SDL_WINDOW_FULLSCREEN);
        const window_flags_t high_pixel_density(SDL_WINDOW_HIGH_PIXEL_DENSITY);
        const window_flags_t hidden(SDL_WINDOW_HIDDEN);
    }

    // ========================================================================
    // Audio Format
    // ========================================================================

    namespace audio_format
    {
        const audio_format_t u8(SDL_AUDIO_U8);
        const audio_format_t s8(SDL_AUDIO_S8);
        const audio_format_t s16(SDL_AUDIO_S16);
        const audio_format_t s32(SDL_AUDIO_S32);
        const audio_format_t f32(SDL_AUDIO_F32);
    }

    // ========================================================================
    // Input Types
    // ========================================================================

    namespace input_action
    {
        const input_action_t release(0); // SDL uses 0 for release
        const input_action_t press(1);   // SDL uses 1 for press
        const input_action_t repeat(2);  // Custom value for key repeat
    }

    namespace input_mod
    {
        const input_mod_t shift(SDL_KMOD_SHIFT);
        const input_mod_t control(SDL_KMOD_CTRL);
        const input_mod_t alt(SDL_KMOD_ALT);
        const input_mod_t super(SDL_KMOD_GUI);
    }

} // namespace sdl
