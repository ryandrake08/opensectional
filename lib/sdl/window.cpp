#include "window.hpp"
#include "error.hpp"
#include "instance.hpp"
#include <SDL3/SDL.h>

namespace sdl
{
    namespace window_flags
    {
        const window_flags_t resizable(SDL_WINDOW_RESIZABLE);
        const window_flags_t borderless(SDL_WINDOW_BORDERLESS);
        const window_flags_t fullscreen(SDL_WINDOW_FULLSCREEN);
        const window_flags_t high_pixel_density(SDL_WINDOW_HIGH_PIXEL_DENSITY);
        const window_flags_t hidden(SDL_WINDOW_HIDDEN);
    }

    namespace
    {
        SDL_Window* create_window(const char* title, int width, int height, SDL_WindowFlags flags)
        {
            SDL_Window* handle = SDL_CreateWindow(title, width, height, flags);
            if(!handle)
            {
                throw error("Failed to create window");
            }

            // Log window information
            SDL_DisplayID display_id = SDL_GetDisplayForWindow(handle);
            const char* display_name = SDL_GetDisplayName(display_id);
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Window created: \"%s\" (%dx%d) on display: %s", title, width,
                        height, display_name ? display_name : "Unknown");
            return handle;
        }
    } // namespace

    void window::release::operator()(SDL_Window* handle) const
    {
        SDL_DestroyWindow(handle);
    }

    window::window(const instance& /* inst */, const char* title, int width, int height, window_flags_t flags)
        : handle_(create_window(title, width, height, static_cast<SDL_WindowFlags>(flags.value)))
    {
    }

    SDL_Window* window::get() const
    {
        return handle_.get();
    }
} // namespace sdl
