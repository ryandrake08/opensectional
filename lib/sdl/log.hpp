#pragma once
#include <string_view>

namespace sdl
{
    // Funnels through SDL_Log* with SDL_LOG_CATEGORY_APPLICATION so
    // priority follows the verbosity set in sdl::instance and output
    // is consistent with the wrapper's own diagnostics. Caller pre-
    // formats the message; embedded % chars are safe.
    void log_info(std::string_view msg);
    void log_warn(std::string_view msg);
    void log_error(std::string_view msg);
    void log_debug(std::string_view msg);

} // namespace sdl
