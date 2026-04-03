#include "instance.hpp"
#include "error.hpp"
#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>

namespace sdl
{
    struct instance::impl
    {
        impl()
        {
#ifdef __linux__
            // Force X11 backend on Linux (avoids Wayland/libdecor/GTK memory leaks)
            SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "x11");
#endif

            if(!SDL_Init(SDL_INIT_VIDEO))
            {
                throw error("Failed to initialize SDL");
            }

            if(!TTF_Init())
            {
                SDL_Quit();
                throw error("Failed to initialize SDL_ttf");
            }

            // Log informational messages, suppress noisy GPU/render categories
            SDL_SetLogPriorities(SDL_LOG_PRIORITY_INFO);
            SDL_SetLogPriority(SDL_LOG_CATEGORY_GPU, SDL_LOG_PRIORITY_WARN);
            SDL_SetLogPriority(SDL_LOG_CATEGORY_RENDER, SDL_LOG_PRIORITY_WARN);
            SDL_SetLogPriority(SDL_LOG_CATEGORY_ERROR, SDL_LOG_PRIORITY_ERROR);

            int version = SDL_GetVersion();
            SDL_Log("SDL3 initialized, version: %d.%d.%d",
                    SDL_VERSIONNUM_MAJOR(version),
                    SDL_VERSIONNUM_MINOR(version),
                    SDL_VERSIONNUM_MICRO(version));

            // Log display information
            int num_displays = 0;
            SDL_DisplayID* displays = SDL_GetDisplays(&num_displays);
            if(displays != nullptr && num_displays > 0)
            {
                SDL_Log("Found %d display(s)", num_displays);
                for(int i = 0; i < num_displays; i++)
                {
                    const char* display_name = SDL_GetDisplayName(displays[i]);
                    SDL_Rect bounds;
                    if(SDL_GetDisplayBounds(displays[i], &bounds))
                    {
                        SDL_Log("  Display %d: %s (%dx%d)", i,
                                display_name ? display_name : "Unknown",
                                bounds.w, bounds.h);
                    }
                }
                SDL_free(displays);
            }

            // Log available GPU drivers
            int num_gpu_drivers = SDL_GetNumGPUDrivers();
            if(num_gpu_drivers > 0)
            {
                SDL_Log("Available GPU drivers: %d", num_gpu_drivers);
                for(int i = 0; i < num_gpu_drivers; i++)
                {
                    const char* driver_name = SDL_GetGPUDriver(i);
                    if(driver_name)
                    {
                        SDL_Log("  GPU driver %d: %s", i, driver_name);
                    }
                }
            }
        }

        ~impl()
        {
            TTF_Quit();
            SDL_Quit();
            SDL_Log("SDL3 terminated");
        }
    };

    instance::instance() : pimpl(new impl)
    {
    }

    instance::~instance() = default;

    instance::instance(instance&& other) noexcept : pimpl(std::move(other.pimpl))
    {
    }

    instance& instance::operator=(instance&& other) noexcept
    {
        if(this != &other)
        {
            pimpl = std::move(other.pimpl);
        }
        return *this;
    }
} // namespace sdl
