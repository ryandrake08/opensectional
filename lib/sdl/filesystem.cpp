#include "filesystem.hpp"
#include <SDL3/SDL_filesystem.h>
#include <sys/stat.h>

namespace sdl
{
    namespace
    {
        bool path_exists(const std::string& p)
        {
            struct stat st{};
            return stat(p.c_str(), &st) == 0;
        }
    }

    std::string base_path()
    {
        const char* p = SDL_GetBasePath();
        return p ? std::string(p) : std::string();
    }

    std::string resolve_bundled_asset(const char* name)
    {
        // SDL_GetBasePath returns a trailing path separator on all platforms
        // it supports, so a plain concatenation works for both / and \.
        std::string base = base_path();
        if(!base.empty())
        {
            std::string candidate = base + name;
            if(path_exists(candidate))
                return candidate;
        }
        if(path_exists(name))
            return name;
        return {};
    }
}
