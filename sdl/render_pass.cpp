#include "render_pass.hpp"
#include "buffer.hpp"
#include "command_buffer.hpp"
#include "error.hpp"
#include "pipeline.hpp"
#include "sampler.hpp"
#include "text.hpp"
#include "texture.hpp"
#include <SDL3/SDL.h>

namespace sdl
{
    // Render pass implementation
    struct render_pass::impl
    {
        SDL_GPURenderPass* handle;        // Owning (ended on destruction)
        SDL_GPUCommandBuffer* cmd_buffer; // Non-owning (for push uniforms)

        static SDL_GPURenderPass* begin_render_pass(SDL_GPUCommandBuffer* command_buffer,
                                                    SDL_GPUTexture* color_target,
                                                    SDL_GPUTexture* depth_target,
                                                    const SDL_FColor& clear_color,
                                                    float clear_depth)
        {
            if(!color_target)
            {
                throw error("Cannot create render pass: color target is null");
            }

            // Set up color target
            SDL_GPUColorTargetInfo color_target_info = {};
            color_target_info.texture = color_target;
            color_target_info.load_op = SDL_GPU_LOADOP_CLEAR;
            color_target_info.store_op = SDL_GPU_STOREOP_STORE;
            color_target_info.clear_color = clear_color;
            color_target_info.cycle = false;

            // Set up depth target if provided
            if(depth_target)
            {
                SDL_GPUDepthStencilTargetInfo depth_target_info = {};
                depth_target_info.texture = depth_target;
                depth_target_info.load_op = SDL_GPU_LOADOP_CLEAR;
                depth_target_info.store_op = SDL_GPU_STOREOP_DONT_CARE;
                depth_target_info.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
                depth_target_info.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
                depth_target_info.clear_depth = clear_depth;
                depth_target_info.cycle = false;
                return SDL_BeginGPURenderPass(command_buffer, &color_target_info, 1, &depth_target_info);
            }

            return SDL_BeginGPURenderPass(command_buffer, &color_target_info, 1, nullptr);
        }

        impl(SDL_GPUCommandBuffer* cmd,
             SDL_GPUTexture* color_target,
             SDL_GPUTexture* depth_target,
             const SDL_FColor& clear_color,
             float clear_depth) : handle(begin_render_pass(cmd, color_target, depth_target, clear_color, clear_depth)),
                                  cmd_buffer(cmd)
        {
            if(!handle)
            {
                throw error("Failed to begin render pass");
            }
        }

        ~impl() noexcept { SDL_EndGPURenderPass(handle); }
    };

    render_pass::render_pass(command_buffer& cmd,
                             const texture& color_target,
                             float clear_r,
                             float clear_g,
                             float clear_b,
                             float clear_a) : pimpl(new impl(cmd.get(),
                                                             color_target.get(),
                                                             nullptr,
                                                             SDL_FColor { clear_r, clear_g, clear_b, clear_a },
                                                             1.0F))
    {
    }

    render_pass::render_pass(command_buffer& cmd,
                             const texture& color_target,
                             const texture& depth_target,
                             float clear_r,
                             float clear_g,
                             float clear_b,
                             float clear_a,
                             float clear_depth) : pimpl(new impl(cmd.get(),
                                                                 color_target.get(),
                                                                 depth_target.get(),
                                                                 SDL_FColor { clear_r, clear_g, clear_b, clear_a },
                                                                 clear_depth))
    {
    }

    render_pass::~render_pass() = default;

    render_pass::render_pass(render_pass&& other) noexcept : pimpl(std::move(other.pimpl))
    {
    }

    render_pass& render_pass::operator=(render_pass&& other) noexcept
    {
        if(this != &other)
        {
            pimpl = std::move(other.pimpl);
        }
        return *this;
    }

    void render_pass::bind_pipeline(const pipeline& pipe)
    {
        SDL_BindGPUGraphicsPipeline(pimpl->handle, pipe.get());
    }

    void render_pass::bind_vertex_buffer(const buffer& buf, uint32_t offset)
    {
        SDL_GPUBufferBinding binding = {};
        binding.buffer = buf.get();
        binding.offset = offset;
        SDL_BindGPUVertexBuffers(pimpl->handle, 0, &binding, 1);
    }

    void render_pass::bind_index_buffer(const buffer& buf, uint32_t offset)
    {
        SDL_GPUBufferBinding binding = {};
        binding.buffer = buf.get();
        binding.offset = offset;
        SDL_BindGPUIndexBuffer(pimpl->handle, &binding, SDL_GPU_INDEXELEMENTSIZE_32BIT);
    }

    void render_pass::push_vertex_uniforms(uint32_t slot, const void* data, uint32_t size)
    {
        SDL_PushGPUVertexUniformData(pimpl->cmd_buffer, slot, data, size);
    }

    void render_pass::push_fragment_uniforms(uint32_t slot, const void* data, uint32_t size)
    {
        SDL_PushGPUFragmentUniformData(pimpl->cmd_buffer, slot, data, size);
    }

    void render_pass::bind_vertex_storage_buffer(uint32_t slot, const buffer& buf)
    {
        SDL_GPUBuffer* buffer_ptr = buf.get();
        SDL_BindGPUVertexStorageBuffers(pimpl->handle, slot, &buffer_ptr, 1);
    }

    void render_pass::bind_fragment_storage_buffer(uint32_t slot, const buffer& buf)
    {
        SDL_GPUBuffer* buffer_ptr = buf.get();
        SDL_BindGPUFragmentStorageBuffers(pimpl->handle, slot, &buffer_ptr, 1);
    }

    void render_pass::bind_fragment_texture_sampler(uint32_t slot, const texture& tex, const sampler& samp)
    {
        SDL_GPUTextureSamplerBinding binding = {};
        binding.texture = tex.get();
        binding.sampler = samp.get();
        SDL_BindGPUFragmentSamplers(pimpl->handle, slot, &binding, 1);
    }

    void render_pass::bind_fragment_texture_sampler(uint32_t slot, const text& txt, const sampler& samp)
    {
        SDL_GPUTexture* atlas = txt.atlas_texture();
        if(atlas)
        {
            SDL_GPUTextureSamplerBinding binding = {};
            binding.texture = atlas;
            binding.sampler = samp.get();
            SDL_BindGPUFragmentSamplers(pimpl->handle, slot, &binding, 1);
        }
    }

    void render_pass::draw(uint32_t vertex_count, uint32_t instance_count, uint32_t first_vertex, uint32_t first_instance)
    {
        SDL_DrawGPUPrimitives(pimpl->handle, vertex_count, instance_count, first_vertex, first_instance);
    }

    void render_pass::draw_indexed(uint32_t index_count, uint32_t instance_count, uint32_t first_index, int32_t vertex_offset, uint32_t first_instance)
    {
        SDL_DrawGPUIndexedPrimitives(pimpl->handle, index_count, instance_count, first_index, vertex_offset, first_instance);
    }

    void render_pass::set_scissor(int x, int y, int w, int h)
    {
        SDL_Rect rect = { x, y, w, h };
        SDL_SetGPUScissor(pimpl->handle, &rect);
    }

    SDL_GPURenderPass* render_pass::get() const
    {
        return pimpl->handle;
    }
} // namespace sdl
