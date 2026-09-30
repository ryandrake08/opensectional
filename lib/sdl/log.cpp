#include "log.hpp"
#include <SDL3/SDL.h>

namespace sdl
{
    namespace
    {
        // %.*s treats the payload as data, so a stray % in the message
        // can't be interpreted as a format directive.
        constexpr auto FMT = "%.*s";
    }

    void log_info(const std::string& msg)
    {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, FMT, static_cast<int>(msg.size()), msg.data());
    }

    void log_warn(const std::string& msg)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, FMT, static_cast<int>(msg.size()), msg.data());
    }

    void log_error(const std::string& msg)
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, FMT, static_cast<int>(msg.size()), msg.data());
    }

    void log_debug(const std::string& msg)
    {
        SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, FMT, static_cast<int>(msg.size()), msg.data());
    }
} // namespace sdl
