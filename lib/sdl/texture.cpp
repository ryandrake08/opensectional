#include "texture.hpp"
#include "device.hpp"
#include "error.hpp"
#include "surface.hpp"
#include <SDL3/SDL.h>

namespace sdl
{
    namespace texture_format
    {
        const texture_format_t r8_unorm(SDL_GPU_TEXTUREFORMAT_R8_UNORM);
        const texture_format_t r8g8_unorm(SDL_GPU_TEXTUREFORMAT_R8G8_UNORM);
        const texture_format_t r8g8b8a8_unorm(SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM);
        const texture_format_t r16_unorm(SDL_GPU_TEXTUREFORMAT_R16_UNORM);
        const texture_format_t r16g16_unorm(SDL_GPU_TEXTUREFORMAT_R16G16_UNORM);
        const texture_format_t r16g16b16a16_unorm(SDL_GPU_TEXTUREFORMAT_R16G16B16A16_UNORM);
        const texture_format_t r10g10b10a2_unorm(SDL_GPU_TEXTUREFORMAT_R10G10B10A2_UNORM);
        const texture_format_t r8g8b8a8_unorm_srgb(SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB);
        const texture_format_t d16_unorm(SDL_GPU_TEXTUREFORMAT_D16_UNORM);
        const texture_format_t d24_unorm(SDL_GPU_TEXTUREFORMAT_D24_UNORM);
        const texture_format_t d32_float(SDL_GPU_TEXTUREFORMAT_D32_FLOAT);
        const texture_format_t d24_unorm_s8_uint(SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT);
        const texture_format_t d32_float_s8_uint(SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT);
    }

    namespace
    {
        SDL_GPUTexture* create_texture(SDL_GPUDevice* dev, const SDL_GPUTextureCreateInfo& info)
        {
            SDL_GPUTexture* handle = SDL_CreateGPUTexture(dev, &info);
            if(!handle)
            {
                throw error("Failed to create GPU texture");
            }

            SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Texture created: %ux%u", info.width, info.height);
            return handle;
        }

        SDL_GPUTextureCreateInfo sampled_texture_create_info(unsigned width, unsigned height, texture_format_t format)
        {
            SDL_GPUTextureCreateInfo info = {};
            info.type = SDL_GPU_TEXTURETYPE_2D;
            info.format = static_cast<SDL_GPUTextureFormat>(format.value);
            info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
            info.width = static_cast<Uint32>(width);
            info.height = static_cast<Uint32>(height);
            info.layer_count_or_depth = 1;
            info.num_levels = 1;
            info.sample_count = SDL_GPU_SAMPLECOUNT_1;
            return info;
        }
    } // namespace

    void texture::release::operator()(SDL_GPUTexture* handle) const
    {
        if(device)
        {
            SDL_ReleaseGPUTexture(device, handle);
        }
    }

    texture::texture(const device& dev, const surface& surf)
        : handle_(create_texture(dev.get(), surf.texture_create_info()), release{dev.get()})
    {
    }

    texture::texture(const device& dev, unsigned width, unsigned height, texture_format_t format)
        : handle_(create_texture(dev.get(), sampled_texture_create_info(width, height, format)), release{dev.get()})
    {
    }

    texture::texture(SDL_GPUTexture* raw_texture) : handle_(raw_texture, release{nullptr})
    {
        if(!handle_)
        {
            throw error("Failed to wrap GPU texture");
        }
    }

    texture::texture(SDL_GPUDevice* dev, SDL_GPUTexture* raw_texture) : handle_(raw_texture, release{dev})
    {
        if(!handle_)
        {
            throw error("Failed to take ownership of GPU texture");
        }
    }

    SDL_GPUTexture* texture::get() const
    {
        return handle_.get();
    }

    int texture::get_image_height(const char* file_path)
    {
        return surface::get_image_height(file_path);
    }
} // namespace sdl
