#include "shader.hpp"
#include "device.hpp"
#include "error.hpp"
#include <SDL3/SDL.h>

namespace sdl
{
    struct shader::impl
    {
        SDL_GPUDevice* device; // Non-owning
        SDL_GPUShader* handle; // Owning

        static SDL_GPUShader* create_shader(
            const sdl::device& dev,
            const void* code,
            size_t code_size,
            const std::string& entrypoint,
            SDL_GPUShaderStage stage,
            SDL_GPUShaderFormat format,
            uint32_t num_samplers,
            uint32_t num_storage_buffers)
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

            return SDL_CreateGPUShader(dev.get(), &shader_info);
        }

        impl(
            const sdl::device& dev,
            const void* code,
            size_t code_size,
            const std::string& entrypoint,
            SDL_GPUShaderStage stage,
            SDL_GPUShaderFormat format,
            uint32_t num_samplers,
            uint32_t num_storage_buffers) : device(dev.get()), handle(create_shader(dev, code, code_size, entrypoint, stage, format, num_samplers, num_storage_buffers))
        {
            if(!handle)
            {
                throw error("Failed to create shader");
            }

            SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Shader created: %s, %lu bytes",
                         stage == SDL_GPU_SHADERSTAGE_VERTEX ? "vertex" : "fragment",
                         (unsigned long)code_size);
        }

        ~impl() noexcept
        {
            SDL_ReleaseGPUShader(device, handle);
        }

        impl(const impl&) = delete;
        impl& operator=(const impl&) = delete;
        impl(impl&&) = default;
        impl& operator=(impl&&) = default;
    };

    shader::shader(
        const device& dev,
        const unsigned char* code_array,
        unsigned int code_len,
        const std::string& entrypoint,
        shader_stage_t stage,
        shader_format_t format,
        uint32_t num_samplers,
        uint32_t num_storage_buffers) : pimpl(new impl(dev,
                                                       static_cast<const void*>(code_array),
                                                       code_len,
                                                       entrypoint,
                                                       static_cast<SDL_GPUShaderStage>(stage.value),
                                                       static_cast<SDL_GPUShaderFormat>(format.value),
                                                       num_samplers,
                                                       num_storage_buffers))
    {
    }

    shader::~shader() = default;

    shader::shader(shader&& other) noexcept : pimpl(std::move(other.pimpl))
    {
    }

    shader& shader::operator=(shader&& other) noexcept
    {
        if(this != &other)
        {
            pimpl = std::move(other.pimpl);
        }
        return *this;
    }

    SDL_GPUShader* shader::get() const
    {
        return pimpl->handle;
    }

} // namespace sdl
