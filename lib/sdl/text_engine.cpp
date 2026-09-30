#include "text_engine.hpp"
#include "device.hpp"
#include "error.hpp"
#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>

namespace sdl
{
    namespace
    {
        TTF_TextEngine* create_text_engine(const device& dev)
        {
            TTF_TextEngine* handle = TTF_CreateGPUTextEngine(dev.get());
            if(!handle)
            {
                throw error("Failed to create GPU text engine");
            }
            return handle;
        }
    } // namespace

    void text_engine::release::operator()(TTF_TextEngine* handle) const
    {
        TTF_DestroyGPUTextEngine(handle);
    }

    text_engine::text_engine(const device& dev) : handle_(create_text_engine(dev))
    {
    }

    TTF_TextEngine* text_engine::get() const
    {
        return handle_.get();
    }
} // namespace sdl
