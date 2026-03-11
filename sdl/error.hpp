#pragma once

#include <stdexcept>
#include <string>

namespace sdl
{
    // Exception that automatically appends SDL_GetError() to the message
    class error : public std::runtime_error
    {
    public:
        explicit error(const char* message);
        explicit error(const std::string& message);
    };

} // namespace sdl
