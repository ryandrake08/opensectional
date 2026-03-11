#pragma once
#include "types.hpp"
#include <memory>

namespace sdl
{
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
        struct impl;
        std::unique_ptr<impl> pimpl;

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
         * @param dev GPU device
         * @param vertex_shader Vertex shader
         * @param fragment_shader Fragment shader
         * @param topology Primitive topology (TRIANGLELIST, TRIANGLESTRIP, or LINELIST)
         * @param depth_format Depth texture format (0 = no depth testing)
         */
        pipeline(const device& dev,
                 shader&& vertex_shader,
                 shader&& fragment_shader,
                 primitive_type_t topology,
                 texture_format_t depth_format = texture_format_t(0));

        /**
         * Destroy pipeline.
         */
        ~pipeline();

        // Non-copyable
        pipeline(const pipeline&) = delete;
        pipeline& operator=(const pipeline&) = delete;

        // Moveable
        pipeline(pipeline&& other) noexcept;
        pipeline& operator=(pipeline&& other) noexcept;

        /**
         * Get underlying SDL_GPUGraphicsPipeline handle.
         *
         * @return Raw pipeline pointer (non-owning)
         */
        SDL_GPUGraphicsPipeline* get() const;
    };

} // namespace sdl
