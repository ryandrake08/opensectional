#include "shader.hpp"
#include "device.hpp"
#include "error.hpp"
#include <SDL3/SDL.h>

namespace sdl
{
    namespace shader_stage
    {
        const shader_stage_t vertex(SDL_GPU_SHADERSTAGE_VERTEX);
        const shader_stage_t fragment(SDL_GPU_SHADERSTAGE_FRAGMENT);
    }

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

    namespace
    {
        SDL_GPUShader* create_shader(const sdl::device& dev, const void* code, size_t code_size,
                                     const std::string& entrypoint, SDL_GPUShaderStage stage,
                                     SDL_GPUShaderFormat format, uint32_t num_samplers, uint32_t num_storage_buffers)
        {
            // Auto-detect format if not specified
            if(format == SDL_GPU_SHADERFORMAT_INVALID)
            {
                format = static_cast<SDL_GPUShaderFormat>(dev.get_shader_format().value);
            }

            // Create shader info
            SDL_GPUShaderCreateInfo shader_info = {};
            shader_info.code = static_cast<const Uint8*>(code);
            shader_info.code_size = code_size;
            shader_info.entrypoint = entrypoint.c_str();
            shader_info.format = format;
            shader_info.stage = stage;
            shader_info.num_samplers = num_samplers;
            shader_info.num_storage_textures = 0;
            shader_info.num_storage_buffers = num_storage_buffers;
            shader_info.num_uniform_buffers = 1; // Most shaders use one uniform buffer

            SDL_GPUShader* handle = SDL_CreateGPUShader(dev.get(), &shader_info);
            if(!handle)
            {
                throw error("Failed to create shader");
            }

            SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Shader created: %s, %lu bytes",
                         stage == SDL_GPU_SHADERSTAGE_VERTEX ? "vertex" : "fragment", (unsigned long)code_size);
            return handle;
        }
    } // namespace

    void shader::release::operator()(SDL_GPUShader* handle) const
    {
        SDL_ReleaseGPUShader(device, handle);
    }

    shader::shader(const device& dev, const unsigned char* code_array, unsigned int code_len,
                   const std::string& entrypoint, shader_stage_t stage, shader_format_t format, uint32_t num_samplers,
                   uint32_t num_storage_buffers)
        : handle_(create_shader(dev, static_cast<const void*>(code_array), code_len, entrypoint,
                                static_cast<SDL_GPUShaderStage>(stage.value),
                                static_cast<SDL_GPUShaderFormat>(format.value), num_samplers, num_storage_buffers),
                  release{dev.get()})
    {
    }

    SDL_GPUShader* shader::get() const
    {
        return handle_.get();
    }

} // namespace sdl
