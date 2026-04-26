#include "timer.hpp"
#include <SDL3/SDL.h>

namespace sdl
{

    timer::timer()
        : start(SDL_GetPerformanceCounter())
    {
    }

    float timer::elapsed_ms() const
    {
        uint64_t now = SDL_GetPerformanceCounter();
        return static_cast<float>(now - start)
            / static_cast<float>(SDL_GetPerformanceFrequency()) * 1000.0F;
    }

} // namespace sdl
