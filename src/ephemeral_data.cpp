#include "ephemeral_data.hpp"
#include "ephemeral_database.hpp"
#include "http_client.hpp"
#include "tfr_source.hpp"
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>

#ifndef OSECT_BUNDLE_IDENTIFIER
#define OSECT_BUNDLE_IDENTIFIER "org.existens.opensectional"
#endif

namespace osect
{
    namespace
    {
        std::string getenv_or_throw(const char* var)
        {
            const char* v = std::getenv(var);
            if(!v || !*v)
            {
                throw std::runtime_error(std::string("environment variable not set: ") + var);
            }
            return v;
        }

        // Per-platform app cache directory. `ephemeral.db` sits at the
        // root of this directory — no further subdirectory, since the
        // database is the only file the app caches there.
        //   macOS:   ~/Library/Caches/<bundle_id>/
        //   Linux:   ${XDG_CACHE_HOME:-$HOME/.cache}/osect/
        //   Windows: %LOCALAPPDATA%/osect/
        // Created if missing; throws if the platform env var isn't set
        // or the directory can't be created.
        std::filesystem::path app_cache_dir()
        {
#if defined(__APPLE__)
            const auto dir = std::filesystem::path(getenv_or_throw("HOME")) / "Library/Caches" /
                             OSECT_BUNDLE_IDENTIFIER;
#elif defined(_WIN32)
            const auto dir = std::filesystem::path(getenv_or_throw("LOCALAPPDATA")) / "osect";
#else
            const char* xdg     = std::getenv("XDG_CACHE_HOME");
            const auto dir      = ((xdg && *xdg) ? std::filesystem::path(xdg)
                                                 : std::filesystem::path(getenv_or_throw("HOME")) / ".cache") /
                                  "osect";
#endif
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            if(ec)
            {
                throw std::runtime_error("failed to create app cache dir '" + dir.string() + "': " + ec.message());
            }
            return dir;
        }
    }

    struct ephemeral_data::impl
    {
        http_client http;
        ephemeral_database db;
        tfr_source tfrs;

        // Last-seen last_updated() per source, for poll_advance().
        std::optional<std::chrono::system_clock::time_point> tfrs_seen;

        explicit impl(bool offline) : http(offline), db(app_cache_dir() / "ephemeral.db"), tfrs(http, db)
        {
        }
    };

    ephemeral_data::ephemeral_data(bool offline) : pimpl(std::make_unique<impl>(offline))
    {
    }

    ephemeral_data::~ephemeral_data() = default;

    const tfr_source& ephemeral_data::tfrs() const
    {
        return pimpl->tfrs;
    }

    std::vector<data_source> ephemeral_data::as_data_sources() const
    {
        return {pimpl->tfrs.as_data_source()};
    }

    bool ephemeral_data::poll_advance()
    {
        bool advanced = false;
        const auto cur = pimpl->tfrs.last_updated();
        if(cur != pimpl->tfrs_seen)
        {
            pimpl->tfrs_seen = cur;
            advanced = true;
        }
        return advanced;
    }
}
