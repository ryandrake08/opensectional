#include "texture.hpp"
#include "device.hpp"
#include "error.hpp"
#include "surface.hpp"
#include <SDL3/SDL.h>

namespace sdl
{
    // Texture implementation
    struct texture::impl
    {
        SDL_GPUDevice* device;  // Non-owning
        SDL_GPUTexture* handle; // Owning or non-owning based on whether device is nullptr

        // Owning constructor - creates texture
        impl(SDL_GPUDevice* dev, const SDL_GPUTextureCreateInfo& info) : device(dev), handle(SDL_CreateGPUTexture(dev, &info))
        {
            if(!handle)
            {
                throw error("Failed to create GPU texture");
            }

            // Log texture creation details
            SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Texture created: %ux%u", info.width, info.height);
        }

        // Non-owning constructor - wraps existing texture
        impl(SDL_GPUTexture* existing_texture) : device(nullptr), handle(existing_texture)
        {
            if(!handle)
            {
                throw error("Failed to wrap GPU texture");
            }
        }

        // Owning constructor - takes ownership of existing texture
        impl(SDL_GPUDevice* dev, SDL_GPUTexture* existing_texture) : device(dev), handle(existing_texture)
        {
            if(!handle)
            {
                throw error("Failed to take ownership of GPU texture");
            }
        }

        ~impl() noexcept
        {
            if(device)
            {
                SDL_ReleaseGPUTexture(device, handle);
            }
        }

        impl(const impl&) = delete;
        impl& operator=(const impl&) = delete;
        impl(impl&&) = default;
        impl& operator=(impl&&) = default;
    };

    texture::texture(const device& dev, const surface& surf) : pimpl(new impl(dev.get(), surf.texture_create_info()))
    {
    }

    texture::texture(SDL_GPUTexture* raw_texture) : pimpl(new impl(raw_texture))
    {
    }

    texture::texture(SDL_GPUDevice* dev, SDL_GPUTexture* raw_texture) : pimpl(new impl(dev, raw_texture))
    {
    }

    texture::~texture() = default;

    texture::texture(texture&& other) noexcept : pimpl(std::move(other.pimpl)) {}

    texture& texture::operator=(texture&& other) noexcept
    {
        if(this != &other)
        {
            pimpl = std::move(other.pimpl);
        }
        return *this;
    }

    SDL_GPUTexture* texture::get() const
    {
        return pimpl->handle;
    }

    int texture::get_image_height(const char* file_path)
    {
        return surface::get_image_height(file_path);
    }
} // namespace sdl
