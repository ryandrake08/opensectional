#include "surface.hpp"
#include "error.hpp"
#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>
#include <string>

namespace sdl
{
    struct surface::impl
    {
        SDL_Surface* handle; // Owning

        static SDL_Surface* load_image(const char* file_path)
        {
            SDL_Surface* loaded_surface = IMG_Load(file_path);
            if(!loaded_surface)
            {
                throw error(std::string("Failed to load image: ") + file_path);
            }

            // Handle grayscale images (treat grayscale as alpha, RGB as white).
            // Paletted images with a color palette are NOT grayscale — they
            // should go through SDL_ConvertSurface to expand the palette.
            bool is_indexed = SDL_ISPIXELFORMAT_INDEXED(loaded_surface->format);
            bool has_palette = is_indexed && SDL_GetSurfacePalette(loaded_surface) != nullptr;
            bool is_grayscale = is_indexed && !has_palette;

            if(is_grayscale)
            {
                SDL_Surface* rgba_surface = SDL_CreateSurface(loaded_surface->w, loaded_surface->h, SDL_PIXELFORMAT_ABGR8888);
                if(!rgba_surface)
                {
                    SDL_DestroySurface(loaded_surface);
                    throw error("Failed to create RGBA surface");
                }

                uint8_t* src = static_cast<uint8_t*>(loaded_surface->pixels);
                uint8_t* dst = static_cast<uint8_t*>(rgba_surface->pixels);

                for(int y = 0; y < loaded_surface->h; y++)
                {
                    for(int x = 0; x < loaded_surface->w; x++)
                    {
                        uint8_t gray = src[y * loaded_surface->pitch + x];
                        int dst_idx = (y * rgba_surface->pitch) + (x * 4);
                        dst[dst_idx + 0] = 255;  // R = white
                        dst[dst_idx + 1] = 255;  // G = white
                        dst[dst_idx + 2] = 255;  // B = white
                        dst[dst_idx + 3] = gray; // A = grayscale value
                    }
                }

                SDL_DestroySurface(loaded_surface);
                SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Surface loaded: %s (%dx%d)", file_path, rgba_surface->w, rgba_surface->h);
                return rgba_surface;
            }

            if(loaded_surface->format != SDL_PIXELFORMAT_ABGR8888)
            {
                SDL_Surface* rgba_surface = SDL_ConvertSurface(loaded_surface, SDL_PIXELFORMAT_ABGR8888);
                SDL_DestroySurface(loaded_surface);
                if(!rgba_surface)
                {
                    throw error("Failed to convert image to RGBA8888");
                }
                SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Surface loaded: %s (%dx%d)", file_path, rgba_surface->w, rgba_surface->h);
                return rgba_surface;
            }

            SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Surface loaded: %s (%dx%d)", file_path, loaded_surface->w, loaded_surface->h);
            return loaded_surface;
        }

        explicit impl(const char* file_path) : handle(load_image(file_path)) {}

        ~impl() noexcept
        {
            if(handle)
            {
                SDL_DestroySurface(handle);
            }
        }
    };

    surface::surface(const char* file_path) : pimpl(new impl(file_path))
    {
    }

    surface::~surface() = default;

    surface::surface(surface&& other) noexcept : pimpl(std::move(other.pimpl))
    {
    }

    surface& surface::operator=(surface&& other) noexcept
    {
        if(this != &other)
        {
            pimpl = std::move(other.pimpl);
        }
        return *this;
    }

    int surface::width() const
    {
        return pimpl->handle->w;
    }

    int surface::height() const
    {
        return pimpl->handle->h;
    }

    uint32_t surface::size() const
    {
        return static_cast<uint32_t>(pimpl->handle->w * pimpl->handle->h * 4); // ABGR8888 = 4 bytes per pixel
    }

    const void* surface::pixels() const
    {
        return pimpl->handle->pixels;
    }

    SDL_Surface* surface::get() const
    {
        return pimpl->handle;
    }

    SDL_GPUTextureCreateInfo surface::texture_create_info() const
    {
        SDL_GPUTextureCreateInfo info = {};
        info.type = SDL_GPU_TEXTURETYPE_2D;
        info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
        info.width = static_cast<uint32_t>(pimpl->handle->w);
        info.height = static_cast<uint32_t>(pimpl->handle->h);
        info.layer_count_or_depth = 1;
        info.num_levels = 1;
        info.sample_count = SDL_GPU_SAMPLECOUNT_1;
        return info;
    }

    int surface::get_image_height(const char* file_path)
    {
        // Load image using SDL3_image
        SDL_Surface* loaded_surface = IMG_Load(file_path);
        if(!loaded_surface)
        {
            throw error(std::string("Failed to load image: ") + file_path);
        }

        int height = loaded_surface->h;
        SDL_DestroySurface(loaded_surface);
        return height;
    }

} // namespace sdl
