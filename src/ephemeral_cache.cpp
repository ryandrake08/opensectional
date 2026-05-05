#include "ephemeral_cache.hpp"
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <stdexcept>

#ifndef OSECT_BUNDLE_IDENTIFIER
#define OSECT_BUNDLE_IDENTIFIER "org.existens.opensectional"
#endif

namespace osect
{
    namespace
    {
        // Versioned binary header. Bumping the version forces a
        // refetch by making prior cache files fail to validate.
        constexpr std::array<char, 4> MAGIC = {'O', 'S', 'E', 'C'};
        constexpr std::uint32_t VERSION = 1;

        // Source names land directly in filenames — reject anything
        // that could escape the cache directory or build an unusual
        // path. Sources are named in code, so this is a programming
        // contract, not user input; throw rather than sanitize.
        void check_source_name(const std::string& name)
        {
            if(name.empty())
            {
                throw std::runtime_error("empty source name");
            }
            for(char c : name)
            {
                const bool ok =
                    (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
                if(!ok)
                {
                    throw std::runtime_error("invalid source name (must be [A-Za-z0-9_-]): '" + name + "'");
                }
            }
        }

        std::filesystem::path file_for(const std::filesystem::path& dir, const std::string& name)
        {
            return dir / (name + ".bin");
        }

        std::string getenv_or_throw(const char* var)
        {
            const char* v = std::getenv(var);
            if(!v || !*v)
            {
                throw std::runtime_error(std::string("environment variable not set: ") + var);
            }
            return v;
        }

        // The cache file is host-private — never copied between
        // machines — so native byte order is fine. read/write the
        // length prefixes verbatim.
        template <typename T>
        void write_pod(std::ofstream& out, T value)
        {
            out.write(reinterpret_cast<const char*>(&value), sizeof(T));
        }

        template <typename T>
        bool read_pod(std::ifstream& in, T& value)
        {
            in.read(reinterpret_cast<char*>(&value), sizeof(T));
            return in.gcount() == sizeof(T);
        }
    }

    std::filesystem::path ephemeral_cache::default_dir()
    {
#if defined(__APPLE__)
        return std::filesystem::path(getenv_or_throw("HOME")) / "Library/Caches" / OSECT_BUNDLE_IDENTIFIER /
               "ephemeral";
#elif defined(_WIN32)
        return std::filesystem::path(getenv_or_throw("LOCALAPPDATA")) / "osect" / "ephemeral";
#else
        const char* xdg = std::getenv("XDG_CACHE_HOME");
        const auto base =
            (xdg && *xdg) ? std::filesystem::path(xdg) : std::filesystem::path(getenv_or_throw("HOME")) / ".cache";
        return base / "osect" / "ephemeral";
#endif
    }

    ephemeral_cache::ephemeral_cache() : ephemeral_cache(default_dir())
    {
    }

    ephemeral_cache::ephemeral_cache(std::filesystem::path cache_dir) : dir_(std::move(cache_dir))
    {
        std::error_code ec;
        std::filesystem::create_directories(dir_, ec);
        if(ec)
        {
            throw std::runtime_error("failed to create cache directory '" + dir_.string() + "': " + ec.message());
        }
    }

    std::optional<ephemeral_cache::entry> ephemeral_cache::load(const std::string& source_name) const
    {
        check_source_name(source_name);
        const auto path = file_for(dir_, source_name);
        std::ifstream in(path, std::ios::binary);
        if(!in)
        {
            return std::nullopt;
        }

        std::array<char, 4> magic{};
        if(!read_pod(in, magic) || magic != MAGIC)
        {
            return std::nullopt;
        }

        std::uint32_t version = 0;
        if(!read_pod(in, version) || version != VERSION)
        {
            return std::nullopt;
        }

        std::uint32_t etag_len = 0;
        if(!read_pod(in, etag_len))
        {
            return std::nullopt;
        }
        // Sanity cap — ETags are short opaque strings, an 8 KiB
        // header would already be unreasonable.
        if(etag_len > 8 * 1024)
        {
            return std::nullopt;
        }
        std::string etag(etag_len, '\0');
        if(etag_len > 0)
        {
            in.read(etag.data(), etag_len);
            if(in.gcount() != static_cast<std::streamsize>(etag_len))
            {
                return std::nullopt;
            }
        }

        std::uint64_t body_len = 0;
        if(!read_pod(in, body_len))
        {
            return std::nullopt;
        }
        // Cap at 256 MiB — bigger than any single ephemeral source we
        // expect to ship; treats a corrupted length as "give up" rather
        // than allocating gigabytes.
        if(body_len > 256ULL * 1024 * 1024)
        {
            return std::nullopt;
        }
        std::string body(body_len, '\0');
        if(body_len > 0)
        {
            in.read(body.data(), static_cast<std::streamsize>(body_len));
            if(static_cast<std::uint64_t>(in.gcount()) != body_len)
            {
                return std::nullopt;
            }
        }

        return entry{std::move(body), std::move(etag)};
    }

    void ephemeral_cache::store(const std::string& source_name, const std::string& body, const std::string& etag)
    {
        check_source_name(source_name);
        if(etag.size() > 8 * 1024)
        {
            throw std::runtime_error("etag too large");
        }

        const auto final_path = file_for(dir_, source_name);
        const auto tmp_path = final_path.string() + ".tmp";

        {
            std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
            if(!out)
            {
                throw std::runtime_error("cannot open cache temp file: " + tmp_path);
            }

            out.write(MAGIC.data(), MAGIC.size());
            write_pod(out, VERSION);
            write_pod(out, static_cast<std::uint32_t>(etag.size()));
            if(!etag.empty())
            {
                out.write(etag.data(), static_cast<std::streamsize>(etag.size()));
            }
            write_pod(out, static_cast<std::uint64_t>(body.size()));
            if(!body.empty())
            {
                out.write(body.data(), static_cast<std::streamsize>(body.size()));
            }
            if(!out)
            {
                throw std::runtime_error("write failed for cache temp file: " + tmp_path);
            }
        } // closes the file before rename

        std::error_code ec;
        std::filesystem::rename(tmp_path, final_path, ec);
        if(ec)
        {
            std::error_code ignore;
            std::filesystem::remove(tmp_path, ignore);
            throw std::runtime_error("rename failed for cache file '" + final_path.string() + "': " + ec.message());
        }
    }

    void ephemeral_cache::clear(const std::string& source_name)
    {
        check_source_name(source_name);
        std::error_code ec;
        std::filesystem::remove(file_for(dir_, source_name), ec);
        // Missing-file is not an error; other errors silently ignored
        // (caller is fine with "best effort" cache deletion).
    }
}
