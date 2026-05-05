#include "window.hpp"
#include "error.hpp"
#include "instance.hpp"
#include <SDL3/SDL.h>

namespace sdl
{
    struct window::impl
    {
        SDL_Window* handle;

        impl(const char* title, int width, int height, SDL_WindowFlags flags)
            : handle(SDL_CreateWindow(title, width, height, flags))
        {
            if(!handle)
            {
                throw error("Failed to create window");
            }

            // Log window information
            SDL_DisplayID display_id = SDL_GetDisplayForWindow(handle);
            const char* display_name = SDL_GetDisplayName(display_id);
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Window created: \"%s\" (%dx%d) on display: %s", title, width,
                        height, display_name ? display_name : "Unknown");
        }

        ~impl() noexcept
        {
            SDL_DestroyWindow(handle);
        }

        impl(const impl&) = delete;
        impl& operator=(const impl&) = delete;
        impl(impl&&) = default;
        impl& operator=(impl&&) = default;
    };

    window::window(const instance& /* inst */, const char* title, int width, int height, window_flags_t flags)
        : pimpl(new impl(title, width, height, static_cast<SDL_WindowFlags>(flags.value)))
    {
    }

    window::~window() = default;

    window::window(window&& other) noexcept : pimpl(std::move(other.pimpl))
    {
    }

    window& window::operator=(window&& other) noexcept
    {
        if(this != &other)
        {
            pimpl = std::move(other.pimpl);
        }
        return *this;
    }

    SDL_Window* window::get() const
    {
        return pimpl->handle;
    }
} // namespace sdl
