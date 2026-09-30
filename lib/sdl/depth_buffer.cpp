#include "depth_buffer.hpp"
#include "device.hpp"
#include "texture.hpp"
#include <SDL3/SDL.h>

namespace sdl
{
    namespace
    {
        texture create_texture(SDL_GPUDevice* device, texture_format_t format, unsigned width, unsigned height)
        {
            SDL_GPUTextureCreateInfo createInfo = {};
            createInfo.type = SDL_GPU_TEXTURETYPE_2D;
            createInfo.format = static_cast<SDL_GPUTextureFormat>(format.value);
            createInfo.width = static_cast<Uint32>(width);
            createInfo.height = static_cast<Uint32>(height);
            createInfo.layer_count_or_depth = 1;
            createInfo.num_levels = 1;
            createInfo.sample_count = SDL_GPU_SAMPLECOUNT_1;
            createInfo.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;

            // Use owning texture constructor with raw device pointer
            return {device, SDL_CreateGPUTexture(device, &createInfo)};
        }
    } // namespace

    depth_buffer::depth_buffer(device& dev, unsigned width, unsigned height, texture_format_t format)
        : device_(dev.get()),
          format_(format),
          width_(width),
          height_(height),
          tex_(create_texture(device_, format_, width_, height_))
    {
    }

    const texture& depth_buffer::get() const
    {
        return tex_;
    }

    texture_format_t depth_buffer::format() const
    {
        return format_;
    }

    void depth_buffer::set_size(unsigned width, unsigned height)
    {
        if(width_ == width && height_ == height)
        {
            return;
        }

        width_ = width;
        height_ = height;
        tex_ = create_texture(device_, format_, width, height);
    }

} // namespace sdl
