#include "filesystem.hpp"
#include <SDL3/SDL_filesystem.h>
#include <cstdlib>
#include <sys/stat.h>

#ifndef OSECT_BUNDLE_IDENTIFIER
#define OSECT_BUNDLE_IDENTIFIER "org.existens.opensectional"
#endif

namespace sdl
{
    namespace
    {
        bool path_exists(const std::string& p)
        {
            struct stat st{};
            return stat(p.c_str(), &st) == 0;
        }

        std::string getenv_or_empty(const char* var)
        {
            const char* v = std::getenv(var);
            return (v && *v) ? std::string(v) : std::string();
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

    std::string resolve_user_asset(const char* name)
    {
#if defined(__APPLE__)
        auto home = getenv_or_empty("HOME");
        if(home.empty()) return {};
        auto candidate = home + "/Library/Application Support/"
            + OSECT_BUNDLE_IDENTIFIER + "/" + name;
#elif defined(_WIN32)
        auto appdata = getenv_or_empty("APPDATA");
        if(appdata.empty()) return {};
        auto candidate = appdata + "\\osect\\" + name;
#else
        auto xdg = getenv_or_empty("XDG_CONFIG_HOME");
        if(xdg.empty())
        {
            auto home = getenv_or_empty("HOME");
            if(home.empty()) return {};
            xdg = home + "/.config";
        }
        auto candidate = xdg + "/osect/" + name;
#endif
        return path_exists(candidate) ? candidate : std::string{};
    }
}
