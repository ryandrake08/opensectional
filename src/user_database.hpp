#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace osect
{
    struct route_record
    {
        std::int64_t route_id;
        std::string name;
        std::string text;
    };

    // SQLite-backed persistence for user-authored data — today,
    // saved flight routes; designed for user-defined waypoints to
    // join as a second schema group later.
    //
    // Lives in the platform-durable application data directory
    // (NOT the cache directory ephemeral.db lives in): user
    // content is not re-fetchable, so an OS-level cache purge
    // silently destroying it would be a data-integrity failure.
    //
    // Schema policy differs deliberately from ephemeral_database:
    // versions are explicit integers (not auto-derived hashes)
    // and mismatches NEVER auto-drop. Missing group → create;
    // on-disk == current → no-op; on-disk < current → walk
    // forward through migrations; on-disk > current → throw.
    // Every schema change must come with a deliberate migration.
    class user_database
    {
        struct impl;
        std::unique_ptr<impl> pimpl;

    public:
        // Per-platform durable path for user.db:
        //   macOS:   $HOME/Library/Application Support/<bundle_id>/user.db
        //   Linux:   ${XDG_DATA_HOME:-$HOME/.local/share}/osect/user.db
        //   Windows: %APPDATA%/osect/user.db
        // Creates the parent directory if missing; throws if the
        // platform env var isn't set or the directory can't be
        // created.
        static std::filesystem::path default_path();

        // Opens db_path read-write, creating the file if missing.
        // Ensures every known group's schema is current; throws
        // on an on-disk version newer than this build understands.
        explicit user_database(const std::filesystem::path& db_path);
        ~user_database();

        user_database(const user_database&) = delete;
        user_database& operator=(const user_database&) = delete;

        // ----- ROUTE -----
        // Every saved route in route_id ascending order — i.e.
        // insertion order, which is also display order until a
        // reorder UI lands.
        std::vector<route_record> load_routes() const;

        // Insert a new route with the given shorthand text.
        // `name` defaults to empty (no UI to set it yet).
        // Returns the assigned route_id.
        std::int64_t insert_route(const std::string& text);

        // Replace the text of an existing route; bumps updated_at.
        // No-op if route_id does not exist.
        void update_route(std::int64_t route_id, const std::string& text);

        // Remove a saved route. No-op if route_id does not exist.
        void delete_route(std::int64_t route_id);

        // Remove every saved route.
        void delete_all_routes();
    };
}
