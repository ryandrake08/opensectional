#pragma once

#include "flight_route.hpp" // route_waypoint_row
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace osect
{
    // A saved route in resolved form: stable identity, display name,
    // and the ordered waypoint rows. Reconstruct a flight_route via
    // flight_route(record.waypoints) — no nasr_database needed.
    struct route_record
    {
        std::int64_t route_id;
        std::string name;
        std::vector<route_waypoint_row> waypoints;
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
    // Routes are stored in resolved form — every waypoint row carries
    // its coordinates — so loading touches no nasr_database.
    //
    // Schema policy: versions are explicit integers (not auto-derived
    // hashes). Missing group → create; on-disk == current → no-op;
    // on-disk > current → throw (refuse to operate on a database
    // newer than this build understands). on-disk < current → today,
    // with no users in the field, the group is simply dropped and
    // recreated; a real forward-migration path goes here once saved
    // routes must survive a schema bump.
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

        // Opens db_path. read_only (the default) opens the file with
        // SQLITE_OPEN_READONLY and skips all schema work — the caller
        // must point it at a database a read-write owner already
        // created and migrated. read_only = false creates the file if
        // missing, ensures every known group's schema is current, and
        // throws on an on-disk version newer than this build
        // understands. Hold a read-only handle as `const user_database`
        // so the mutator API is a compile error rather than a
        // runtime SQLITE_READONLY.
        explicit user_database(const std::filesystem::path& db_path, bool read_only = true);
        ~user_database();

        user_database(const user_database&) = delete;
        user_database& operator=(const user_database&) = delete;

        // ----- ROUTE -----
        // Every saved route in route_id ascending order — i.e.
        // insertion order, which is also display order until a
        // reorder UI lands.
        std::vector<route_record> load_routes() const;

        // Single-row lookup by id. nullopt if no row matches.
        std::optional<route_record> query_route(std::int64_t route_id) const;

        // Insert a new route from its resolved waypoint rows. `name`
        // starts empty (no UI to set it yet). Returns the assigned
        // route_id. Route + waypoints are written in one transaction.
        std::int64_t insert_route(const std::vector<route_waypoint_row>& waypoints);

        // Replace an existing route's waypoints; bumps updated_at.
        // No-op if route_id does not exist.
        void update_route(std::int64_t route_id, const std::vector<route_waypoint_row>& waypoints);

        // Remove a saved route. Its waypoint rows cascade away. No-op
        // if route_id does not exist.
        void delete_route(std::int64_t route_id);
    };
}
