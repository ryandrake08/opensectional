#pragma once
#include "opaque_typedef.hpp"
#include "texture.hpp"
#include <cstdint>
#include <memory>

struct SDL_GPUDevice;
struct SDL_GPUGraphicsPipeline;

namespace sdl
{
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
    // Vertex Structure
    // ========================================================================
    struct vertex_t2f_c4ub_v3f
    {
        float s, t;         // Texture coordinates (8 bytes)
        uint8_t r, g, b, a; // Color (4 bytes)
        float x, y, z;      // Position (12 bytes)
    };

    class device;
    class shader;

    /**
     * RAII wrapper for SDL_GPUGraphicsPipeline
     *
     * Manages graphics pipeline creation and cleanup.
     * Combines shaders with rasterizer, blend, and depth state.
     * Non-copyable, moveable.
     */
    class pipeline
    {
        // Releases the handle on the device that created it.
        struct release
        {
            SDL_GPUDevice* device;
            void operator()(SDL_GPUGraphicsPipeline* handle) const;
        };

        std::unique_ptr<SDL_GPUGraphicsPipeline, release> handle_;

    public:
        /**
         * Create graphics pipeline from vertex and fragment shaders.
         *
         * Pipeline settings are standardized for the common case:
         * - Vertex format: vertex_t2f_c4ub_v3f (texcoord, color, position)
         * - Color target: swapchain format
         * - Blending: standard alpha blending
         * - Depth test: enabled if depth_format is non-zero
         * - Culling: none
         *
         * If vertex_input is false, the vertex input state is empty (for shaders
         * that generate vertices procedurally from SV_VertexID).
         *
         * @param dev GPU device
         * @param vertex_shader Vertex shader
         * @param fragment_shader Fragment shader
         * @param topology Primitive topology (TRIANGLELIST, TRIANGLESTRIP, or LINELIST)
         * @param depth_format Depth texture format (0 = no depth testing)
         * @param vertex_input Whether to use standard vertex input (default: true)
         */
        pipeline(const device& dev, shader&& vertex_shader, shader&& fragment_shader, primitive_type_t topology,
                 texture_format_t depth_format = texture_format_t(0), bool vertex_input = true);

        // Non-copyable
        pipeline(const pipeline&) = delete;
        pipeline& operator=(const pipeline&) = delete;

        // Moveable
        pipeline(pipeline&& other) noexcept = default;
        pipeline& operator=(pipeline&& other) noexcept = default;

        /**
         * Get underlying SDL_GPUGraphicsPipeline handle.
         *
         * @return Raw pipeline pointer (non-owning)
         */
        SDL_GPUGraphicsPipeline* get() const;
    };

} // namespace sdl
