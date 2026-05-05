#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace osect
{
    // On-disk cache backing offline mode and warm starts. One file
    // per source, named for the source ("tfr.bin", "notam.bin", …).
    // Each file carries a small versioned header plus the response
    // ETag plus the response body — enough for the next refresh to
    // issue a conditional GET and for `--offline` runs to display
    // last-known data without network.
    //
    // load() returns nullopt for missing or malformed files; the
    // caller treats both as "no prior data" so a corrupted cache
    // never crashes the app. store() is atomic — writes go through
    // a `.tmp` sidecar and a single rename.
    class ephemeral_cache
    {
    public:
        // Construct against the default per-platform directory
        // (see default_dir()). Creates the directory if missing;
        // throws std::runtime_error if the path can't be created.
        ephemeral_cache();

        // Construct against an explicit directory. Used by tests to
        // sandbox the cache into a tmp path.
        explicit ephemeral_cache(std::filesystem::path cache_dir);

        struct entry
        {
            std::string body;
            std::string etag;
        };

        std::optional<entry> load(const std::string& source_name) const;
        void store(const std::string& source_name, const std::string& body, const std::string& etag);
        void clear(const std::string& source_name);

        // Per-platform default cache directory:
        //   macOS:   ~/Library/Caches/<bundle_id>/ephemeral/
        //   Linux:   ${XDG_CACHE_HOME:-$HOME/.cache}/osect/ephemeral/
        //   Windows: %LOCALAPPDATA%/osect/ephemeral/
        // Throws std::runtime_error if the relevant environment
        // variables are unset (HOME / LOCALAPPDATA).
        static std::filesystem::path default_dir();

        const std::filesystem::path& dir() const
        {
            return dir_;
        }

    private:
        std::filesystem::path dir_;
    };
}
