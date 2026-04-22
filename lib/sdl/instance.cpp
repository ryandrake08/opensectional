#include "instance.hpp"
#include "error.hpp"
#include <array>
#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>

namespace sdl
{
    struct instance::impl
    {
        explicit impl(int verbosity)
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

            // Map verbosity level to SDL log priority
            static const std::array<SDL_LogPriority, 4> levels = {{
                SDL_LOG_PRIORITY_ERROR,  // 0: errors only
                SDL_LOG_PRIORITY_WARN,   // 1: warnings
                SDL_LOG_PRIORITY_INFO,   // 2: info
                SDL_LOG_PRIORITY_DEBUG,  // 3: debug
            }};
            int clamped = verbosity;
            if(clamped < 0)
                clamped = 0;
            else if(clamped > 3)
                clamped = 3;
            SDL_SetLogPriorities(levels[clamped]);

            int version = SDL_GetVersion();
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "SDL3 initialized, version: %d.%d.%d",
                        SDL_VERSIONNUM_MAJOR(version),
                        SDL_VERSIONNUM_MINOR(version),
                        SDL_VERSIONNUM_MICRO(version));

            // Display enumeration (debug-only)
            int num_displays = 0;
            SDL_DisplayID* displays = SDL_GetDisplays(&num_displays);
            if(displays != nullptr && num_displays > 0)
            {
                SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Found %d display(s)", num_displays);
                for(int i = 0; i < num_displays; i++)
                {
                    const char* display_name = SDL_GetDisplayName(displays[i]);
                    SDL_Rect bounds;
                    if(SDL_GetDisplayBounds(displays[i], &bounds))
                    {
                        SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "  Display %d: %s (%dx%d)", i,
                                display_name ? display_name : "Unknown",
                                bounds.w, bounds.h);
                    }
                }
                SDL_free(displays);
            }

            // GPU driver enumeration (debug-only)
            int num_gpu_drivers = SDL_GetNumGPUDrivers();
            if(num_gpu_drivers > 0)
            {
                SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Available GPU drivers: %d", num_gpu_drivers);
                for(int i = 0; i < num_gpu_drivers; i++)
                {
                    const char* driver_name = SDL_GetGPUDriver(i);
                    if(driver_name)
                    {
                        SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "  GPU driver %d: %s", i, driver_name);
                    }
                }
            }
        }

        ~impl()
        {
            TTF_Quit();
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "SDL3 terminated");
            SDL_Quit();
        }

        impl(const impl&) = delete;
        impl& operator=(const impl&) = delete;
        impl(impl&&) = default;
        impl& operator=(impl&&) = default;
    };

    instance::instance(int verbosity) : pimpl(new impl(verbosity))
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
