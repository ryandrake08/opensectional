#include "error.hpp"
#include <SDL3/SDL.h>

namespace sdl
{
    error::error(const char* message) : std::runtime_error(std::string(message) + ": " + SDL_GetError())
    {
    }

    error::error(const std::string& message) : std::runtime_error(message + ": " + SDL_GetError())
    {
    }
} // namespace sdl
