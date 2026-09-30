#pragma once
#include <string>

namespace sdl
{
    // Funnels through SDL_Log* with SDL_LOG_CATEGORY_APPLICATION so
    // priority follows the verbosity set in sdl::instance and output
    // is consistent with the wrapper's own diagnostics. Caller pre-
    // formats the message; embedded % chars are safe.
    void log_info(const std::string& msg);
    void log_warn(const std::string& msg);
    void log_error(const std::string& msg);
    void log_debug(const std::string& msg);

} // namespace sdl
