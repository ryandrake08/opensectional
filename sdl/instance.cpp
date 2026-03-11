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
            if(!SDL_Init(SDL_INIT_VIDEO))
            {
                throw error("Failed to initialize SDL");
            }

            if(!TTF_Init())
            {
                SDL_Quit();
                throw error("Failed to initialize SDL_ttf");
            }

            SDL_SetLogPriorities(SDL_LOG_PRIORITY_INFO);
            SDL_SetLogPriority(SDL_LOG_CATEGORY_GPU, SDL_LOG_PRIORITY_WARN);
            SDL_SetLogPriority(SDL_LOG_CATEGORY_RENDER, SDL_LOG_PRIORITY_WARN);
            SDL_SetLogPriority(SDL_LOG_CATEGORY_ERROR, SDL_LOG_PRIORITY_ERROR);

            int version = SDL_GetVersion();
            SDL_Log("SDL3 initialized, version: %d.%d.%d",
                    SDL_VERSIONNUM_MAJOR(version),
                    SDL_VERSIONNUM_MINOR(version),
                    SDL_VERSIONNUM_MICRO(version));
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
