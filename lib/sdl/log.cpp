#include "log.hpp"
#include <SDL3/SDL.h>

namespace sdl
{
    namespace
    {
        // %.*s avoids requiring a null-terminated buffer and treats the
        // payload as data (so a stray % in the message can't be
        // interpreted as a format directive).
        constexpr auto FMT = "%.*s";
    }

    void log_info(std::string_view msg)
    {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, FMT, static_cast<int>(msg.size()), msg.data());
    }

    void log_warn(std::string_view msg)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, FMT, static_cast<int>(msg.size()), msg.data());
    }

    void log_error(std::string_view msg)
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, FMT, static_cast<int>(msg.size()), msg.data());
    }

    void log_debug(std::string_view msg)
    {
        SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, FMT, static_cast<int>(msg.size()), msg.data());
    }
} // namespace sdl
