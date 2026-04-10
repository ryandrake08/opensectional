#include "font.hpp"
#include "error.hpp"
#include "text_engine.hpp"
#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>
#include <string>

namespace sdl
{
    struct font::impl
    {
        TTF_Font* handle; // Owning

        impl(const char* path, int ptsize) : handle(nullptr)
        {
            if(!path)
            {
                throw error("Cannot load font: path is null");
            }

            handle = TTF_OpenFont(path, ptsize);
            if(!handle)
            {
                throw error(std::string("Failed to load font '") + path);
            }

            // Log font loading information
            SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Font loaded: %s (%dpt)", path, ptsize);
        }

        impl(const void* data, size_t size, int ptsize) : handle(nullptr)
        {
            SDL_IOStream* io = SDL_IOFromConstMem(data, size);
            if(!io)
            {
                throw error("Failed to create IOStream from font data");
            }

            handle = TTF_OpenFontIO(io, true, static_cast<float>(ptsize));
            if(!handle)
            {
                throw error("Failed to load embedded font");
            }

            SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Font loaded from memory (%dpt)", ptsize);
        }

        ~impl() noexcept
        {
            TTF_CloseFont(handle);
        }
    };

    font::font(const text_engine& /* engine */, const char* path, int ptsize) : pimpl(new impl(path, ptsize))
    {
        // Note: engine parameter is unused, but enforces initialization order at compile time
    }

    font::font(const text_engine& /* engine */, const void* data, size_t size, int ptsize) : pimpl(new impl(data, size, ptsize))
    {
    }

    font::~font() = default;

    font::font(font&& other) noexcept : pimpl(std::move(other.pimpl))
    {
    }

    font& font::operator=(font&& other) noexcept
    {
        if(this != &other)
        {
            pimpl = std::move(other.pimpl);
        }
        return *this;
    }

    TTF_Font* font::get() const
    {
        return pimpl->handle;
    }

    void font::set_outline(int outline)
    {
        TTF_SetFontOutline(pimpl->handle, outline);
    }

    int font::get_outline() const
    {
        return TTF_GetFontOutline(pimpl->handle);
    }
} // namespace sdl
