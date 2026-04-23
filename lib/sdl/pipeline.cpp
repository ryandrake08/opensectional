#include "pipeline.hpp"
#include "device.hpp"
#include "error.hpp"
#include "shader.hpp"
#include <array>
#include <SDL3/SDL.h>
#include <cstddef>
#include <memory>

namespace sdl
{
    // Pipeline implementation
    struct pipeline::impl
    {
        SDL_GPUDevice* device;           // Non-owning
        SDL_GPUGraphicsPipeline* handle; // Owning

        static SDL_GPUGraphicsPipeline* create_pipeline(
            SDL_GPUDevice* dev,
            SDL_GPUShader* vs,
            SDL_GPUShader* fs,
            SDL_GPUTextureFormat swapchain_format,
            SDL_GPUPrimitiveType topology,
            SDL_GPUTextureFormat depth_format,
            bool use_vertex_input)
        {
            // Set up standard vertex attributes for vertex_t2f_c4ub_v3f
            std::array<SDL_GPUVertexAttribute, 3> attributes = {};
            attributes[0].location = 0;
            attributes[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
            attributes[0].offset = offsetof(vertex_t2f_c4ub_v3f, s);
            attributes[0].buffer_slot = 0;
            attributes[1].location = 1;
            attributes[1].format = SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM;
            attributes[1].offset = offsetof(vertex_t2f_c4ub_v3f, r);
            attributes[1].buffer_slot = 0;
            attributes[2].location = 2;
            attributes[2].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
            attributes[2].offset = offsetof(vertex_t2f_c4ub_v3f, x);
            attributes[2].buffer_slot = 0;

            // Set up vertex buffer layout
            SDL_GPUVertexBufferDescription vertex_buffer_desc = {};
            vertex_buffer_desc.slot = 0;
            vertex_buffer_desc.pitch = sizeof(vertex_t2f_c4ub_v3f);
            vertex_buffer_desc.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
            vertex_buffer_desc.instance_step_rate = 0;

            // Set up vertex input state
            SDL_GPUVertexInputState vertex_input_state = {};
            if(use_vertex_input)
            {
                vertex_input_state.vertex_buffer_descriptions = &vertex_buffer_desc;
                vertex_input_state.num_vertex_buffers = 1;
                vertex_input_state.vertex_attributes = attributes.data();
                vertex_input_state.num_vertex_attributes = 3;
            }

            // Set up color target state with alpha blending
            SDL_GPUColorTargetDescription color_target = {};
            color_target.format = swapchain_format;
            color_target.blend_state.enable_blend = true;
            color_target.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
            color_target.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
            color_target.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
            color_target.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
            color_target.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
            color_target.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
            color_target.blend_state.color_write_mask = 0xF; // RGBA

            // Set up rasterizer state (no culling)
            SDL_GPURasterizerState rasterizer_state = {};
            rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
            rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
            rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
            rasterizer_state.enable_depth_bias = false;
            rasterizer_state.enable_depth_clip = true;

            // Set up depth stencil state (enabled if depth_format is non-zero)
            bool has_depth = (depth_format != SDL_GPU_TEXTUREFORMAT_INVALID);
            SDL_GPUDepthStencilState depth_stencil_state = {};
            depth_stencil_state.enable_depth_test = has_depth;
            depth_stencil_state.enable_depth_write = has_depth;
            depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
            depth_stencil_state.enable_stencil_test = false;

            // Set up graphics pipeline create info
            SDL_GPUGraphicsPipelineCreateInfo info = {};
            info.vertex_shader = vs;
            info.fragment_shader = fs;
            info.vertex_input_state = vertex_input_state;
            info.primitive_type = topology;
            info.rasterizer_state = rasterizer_state;
            info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
            info.depth_stencil_state = depth_stencil_state;
            info.target_info.num_color_targets = 1;
            info.target_info.color_target_descriptions = &color_target;
            info.target_info.has_depth_stencil_target = has_depth;
            info.target_info.depth_stencil_format = depth_format;

            return SDL_CreateGPUGraphicsPipeline(dev, &info);
        }

        impl(
            SDL_GPUDevice* dev,
            shader&& vs,  // NOLINT(cppcoreguidelines-rvalue-reference-param-not-moved) — consumed at scope end, not stored
            shader&& fs,  // NOLINT(cppcoreguidelines-rvalue-reference-param-not-moved)
            SDL_GPUTextureFormat swapchain_format,
            SDL_GPUPrimitiveType topology,
            SDL_GPUTextureFormat depth_format,
            bool use_vertex_input) : device(dev),
                                     handle(create_pipeline(dev, vs.get(), fs.get(), swapchain_format, topology, depth_format, use_vertex_input))
        {
            if(!handle)
            {
                throw error("Failed to create graphics pipeline");
            }

            const auto* topo_name = [&]
            {
                switch(topology)
                {
                case SDL_GPU_PRIMITIVETYPE_TRIANGLELIST:  return "triangle_list";
                case SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP: return "triangle_strip";
                case SDL_GPU_PRIMITIVETYPE_LINELIST:      return "line_list";
                case SDL_GPU_PRIMITIVETYPE_LINESTRIP:     return "line_strip";
                case SDL_GPU_PRIMITIVETYPE_POINTLIST:     return "point_list";
                default:                                  return "unknown";
                }
            }();
            SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Pipeline created: %s, depth: %s",
                         topo_name, (depth_format != SDL_GPU_TEXTUREFORMAT_INVALID) ? "yes" : "no");
        }

        ~impl() noexcept
        {
            SDL_ReleaseGPUGraphicsPipeline(device, handle);
        }

        impl(const impl&) = delete;
        impl& operator=(const impl&) = delete;
        impl(impl&&) = default;
        impl& operator=(impl&&) = default;
    };

    pipeline::~pipeline() = default;

    pipeline::pipeline(pipeline&& other) noexcept : pimpl(std::move(other.pimpl))
    {
    }

    pipeline& pipeline::operator=(pipeline&& other) noexcept
    {
        if(this != &other)
        {
            pimpl = std::move(other.pimpl);
        }
        return *this;
    }

    SDL_GPUGraphicsPipeline* pipeline::get() const
    {
        return pimpl->handle;
    }

    // Constructor from vertex and fragment shaders
    pipeline::pipeline(
        const device& dev,
        shader&& vertex_shader,
        shader&& fragment_shader,
        primitive_type_t topology,
        texture_format_t depth_format,
        bool vertex_input) : pimpl(new impl(dev.get(),
                                            std::move(vertex_shader),
                                            std::move(fragment_shader),
                                            static_cast<SDL_GPUTextureFormat>(dev.get_swapchain_format().value),
                                            static_cast<SDL_GPUPrimitiveType>(topology.value),
                                            static_cast<SDL_GPUTextureFormat>(depth_format.value),
                                            vertex_input))
    {
    }
} // namespace sdl
