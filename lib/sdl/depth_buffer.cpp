#include "depth_buffer.hpp"
#include "device.hpp"
#include "texture.hpp"
#include <SDL3/SDL.h>

namespace sdl
{
    struct depth_buffer::impl
    {
        SDL_GPUDevice* device; // Non-owning
        SDL_GPUTextureFormat fmt;
        Uint32 width;
        Uint32 height;
        texture tex;

        static texture create_texture(SDL_GPUDevice* device, SDL_GPUTextureFormat format, Uint32 width, Uint32 height)
        {
            SDL_GPUTextureCreateInfo createInfo = {};
            createInfo.type = SDL_GPU_TEXTURETYPE_2D;
            createInfo.format = format;
            createInfo.width = width;
            createInfo.height = height;
            createInfo.layer_count_or_depth = 1;
            createInfo.num_levels = 1;
            createInfo.sample_count = SDL_GPU_SAMPLECOUNT_1;
            createInfo.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;

            // Use owning texture constructor with raw device pointer
            return {device, SDL_CreateGPUTexture(device, &createInfo)};
        }

        impl(SDL_GPUDevice* dev, Uint32 w, Uint32 h, SDL_GPUTextureFormat format)
            : device(dev), fmt(format), width(w), height(h), tex(create_texture(dev, fmt, width, height))
        {
        }
    };

    depth_buffer::depth_buffer(device& dev, unsigned width, unsigned height, texture_format_t format)
        : pimpl(new impl(dev.get(), static_cast<Uint32>(width), static_cast<Uint32>(height),
                         static_cast<SDL_GPUTextureFormat>(format.value)))
    {
    }

    depth_buffer::~depth_buffer() = default;

    depth_buffer::depth_buffer(depth_buffer&& other) noexcept : pimpl(std::move(other.pimpl))
    {
    }

    depth_buffer& depth_buffer::operator=(depth_buffer&& other) noexcept
    {
        if(this != &other)
        {
            pimpl = std::move(other.pimpl);
        }
        return *this;
    }

    const texture& depth_buffer::get() const
    {
        return pimpl->tex;
    }

    texture_format_t depth_buffer::format() const
    {
        return texture_format_t(pimpl->fmt);
    }

    void depth_buffer::set_size(unsigned width, unsigned height)
    {
        auto w = static_cast<Uint32>(width);
        auto h = static_cast<Uint32>(height);

        if(pimpl->width == w && pimpl->height == h)
        {
            return;
        }

        pimpl->width = w;
        pimpl->height = h;
        pimpl->tex = impl::create_texture(pimpl->device, pimpl->fmt, w, h);
    }

} // namespace sdl
