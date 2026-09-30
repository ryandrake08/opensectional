#include "font.hpp"
#include "error.hpp"
#include "text_engine.hpp"
#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>
#include <string>

namespace sdl
{
    namespace
    {
        TTF_Font* load_from_path(const char* path, int ptsize)
        {
            if(!path)
            {
                throw error("Cannot load font: path is null");
            }

            TTF_Font* font = TTF_OpenFont(path, ptsize);
            if(!font)
            {
                throw error(std::string("Failed to load font '") + path);
            }

            SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Font loaded: %s (%dpt)", path, ptsize);
            return font;
        }

        TTF_Font* load_from_memory(const void* data, size_t size, int ptsize)
        {
            SDL_IOStream* io = SDL_IOFromConstMem(data, size);
            if(!io)
            {
                throw error("Failed to create IOStream from font data");
            }

            TTF_Font* font = TTF_OpenFontIO(io, true, static_cast<float>(ptsize));
            if(!font)
            {
                throw error("Failed to load embedded font");
            }

            SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Font loaded from memory (%dpt)", ptsize);
            return font;
        }
    } // namespace

    void font::release::operator()(TTF_Font* handle) const
    {
        TTF_CloseFont(handle);
    }

    font::font(const text_engine& /* engine */, const char* path, int ptsize) : handle_(load_from_path(path, ptsize))
    {
        // Note: engine parameter is unused, but enforces initialization order at compile time
    }

    font::font(const text_engine& /* engine */, const void* data, size_t size, int ptsize)
        : handle_(load_from_memory(data, size, ptsize))
    {
    }

    TTF_Font* font::get() const
    {
        return handle_.get();
    }

    void font::set_outline(int outline)
    {
        TTF_SetFontOutline(handle_.get(), outline);
    }

    int font::get_outline() const
    {
        return TTF_GetFontOutline(handle_.get());
    }
} // namespace sdl
