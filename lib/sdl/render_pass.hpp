#pragma once

#include "types.hpp"
#include <memory>

namespace sdl
{
    class command_buffer;
    class device;
    class pipeline;
    class pipeline;
    class buffer;
    class texture;
    class sampler;
    class text;
    class text_engine;

    /**
     * RAII wrapper for SDL_GPURenderPass
     *
     * Begins render pass on construction, ends on destruction.
     * Non-copyable, non-moveable.
     */
    class render_pass
    {
        struct impl;
        std::unique_ptr<impl> pimpl;

    public:
        /**
         * Begin render pass with single color target.
         *
         * @param cmd Command buffer
         * @param color_target Color target texture
         * @param clear_r Clear color red component (default: 0)
         * @param clear_g Clear color green component (default: 0)
         * @param clear_b Clear color blue component (default: 0)
         * @param clear_a Clear color alpha component (default: 1)
         * @throws std::runtime_error if render pass creation fails
         */
        render_pass(command_buffer& cmd, const texture& color_target, float clear_r = 0.0F, float clear_g = 0.0F,
                    float clear_b = 0.0F, float clear_a = 1.0F);

        /**
         * Begin render pass with color and depth targets.
         *
         * @param cmd Command buffer
         * @param color_target Color target texture
         * @param depth_target Depth target texture
         * @param clear_r Clear color red component (default: 0)
         * @param clear_g Clear color green component (default: 0)
         * @param clear_b Clear color blue component (default: 0)
         * @param clear_a Clear color alpha component (default: 1)
         * @param clear_depth Clear depth value (default: 1.0 = far)
         * @throws std::runtime_error if render pass creation fails
         */
        render_pass(command_buffer& cmd, const texture& color_target, const texture& depth_target, float clear_r = 0.0F,
                    float clear_g = 0.0F, float clear_b = 0.0F, float clear_a = 1.0F, float clear_depth = 1.0F);

        /**
         * End render pass.
         */
        ~render_pass();

        // Non-copyable
        render_pass(const render_pass&) = delete;
        render_pass& operator=(const render_pass&) = delete;

        // Moveable
        render_pass(render_pass&& other) noexcept;
        render_pass& operator=(render_pass&& other) noexcept;

        /**
         * Bind graphics pipeline.
         *
         * @param pipe Pipeline to bind
         */
        void bind_pipeline(const pipeline& pipe);

        /**
         * Bind vertex buffer.
         *
         * @param buf Vertex buffer
         * @param offset Offset in buffer
         */
        void bind_vertex_buffer(const buffer& buf, uint32_t offset = 0);

        /**
         * Bind index buffer.
         *
         * @param buf Index buffer
         * @param offset Offset in buffer
         * @param index_type Index element type (16 or 32 bit)
         */
        void bind_index_buffer(const buffer& buf, uint32_t offset = 0);

        /**
         * Push uniform data to vertex stage.
         *
         * Note: These are command buffer operations, but are provided on render_pass
         * because they can only be called during an active render pass.
         *
         * @param slot Uniform slot (usually 0)
         * @param data Uniform data pointer
         * @param size Data size in bytes
         */
        void push_vertex_uniforms(uint32_t slot, const void* data, uint32_t size);

        /**
         * Push uniform data to fragment stage.
         *
         * Note: These are command buffer operations, but are provided on render_pass
         * because they can only be called during an active render pass.
         *
         * @param slot Uniform slot (usually 0)
         * @param data Uniform data pointer
         * @param size Data size in bytes
         */
        void push_fragment_uniforms(uint32_t slot, const void* data, uint32_t size);

        /**
         * Bind storage buffer to vertex shader.
         *
         * @param slot Binding slot
         * @param buf Storage buffer (must be created with graphics_storage_read usage)
         */
        void bind_vertex_storage_buffer(uint32_t slot, const buffer& buf);

        /**
         * Bind storage buffer to fragment shader.
         *
         * @param slot Binding slot
         * @param buf Storage buffer (must be created with graphics_storage_read usage)
         */
        void bind_fragment_storage_buffer(uint32_t slot, const buffer& buf);

        /**
         * Bind texture and sampler to fragment shader.
         *
         * Convenience method for binding both at the same slot.
         *
         * @param slot Binding slot
         * @param tex Texture to bind
         * @param samp Sampler to bind
         */
        void bind_fragment_texture_sampler(uint32_t slot, const texture& tex, const sampler& samp);

        /**
         * Bind text atlas texture and sampler to fragment shader.
         *
         * Convenience method for binding text atlas texture at the same slot.
         * Automatically extracts the atlas texture from the text object.
         *
         * @param slot Binding slot
         * @param txt Text object (atlas texture is extracted)
         * @param samp Sampler to bind
         */
        void bind_fragment_texture_sampler(uint32_t slot, const text& txt, const sampler& samp);

        /**
         * Draw primitives.
         *
         * @param vertex_count Number of vertices
         * @param instance_count Number of instances (default: 1)
         * @param first_vertex First vertex index (default: 0)
         * @param first_instance First instance index (default: 0)
         */
        void draw(uint32_t vertex_count, uint32_t instance_count = 1, uint32_t first_vertex = 0,
                  uint32_t first_instance = 0);

        /**
         * Draw indexed primitives.
         *
         * @param index_count Number of indices
         * @param instance_count Number of instances (default: 1)
         * @param first_index First index (default: 0)
         * @param vertex_offset Vertex offset added to index (default: 0)
         * @param first_instance First instance index (default: 0)
         */
        void draw_indexed(uint32_t index_count, uint32_t instance_count = 1, uint32_t first_index = 0,
                          int32_t vertex_offset = 0, uint32_t first_instance = 0);

        /**
         * Set scissor rectangle for clipping.
         *
         * @param x X coordinate of scissor rectangle
         * @param y Y coordinate of scissor rectangle
         * @param w Width of scissor rectangle
         * @param h Height of scissor rectangle
         */
        void set_scissor(int x, int y, int w, int h);

        /**
         * Get underlying render pass handle.
         *
         * @return Raw render pass pointer (non-owning)
         */
        SDL_GPURenderPass* get() const;
    };

} // namespace sdl
