#include "text_engine.hpp"
#include "device.hpp"
#include "error.hpp"
#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>

namespace sdl
{
    struct text_engine::impl
    {
        TTF_TextEngine* handle; // Owning

        explicit impl(const device& dev) : handle(TTF_CreateGPUTextEngine(dev.get()))
        {
            if(!handle)
            {
                throw error("Failed to create GPU text engine");
            }
        }

        ~impl() noexcept
        {
            TTF_DestroyGPUTextEngine(handle);
        }
    };

    text_engine::text_engine(const device& dev) : pimpl(new impl(dev)) {}

    text_engine::~text_engine() = default;

    text_engine::text_engine(text_engine&& other) noexcept : pimpl(std::move(other.pimpl)) {}

    text_engine& text_engine::operator=(text_engine&& other) noexcept
    {
        if(this != &other)
        {
            pimpl = std::move(other.pimpl);
        }
        return *this;
    }

    TTF_TextEngine* text_engine::get() const
    {
        return pimpl->handle;
    }
} // namespace sdl
