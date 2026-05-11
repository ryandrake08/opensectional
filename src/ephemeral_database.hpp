#pragma once

#include "tfr.hpp"
#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace osect
{
    // SQLite-backed persistence for runtime-fetched (ephemeral) data
    // sources. Parallel to nasr_database, but opened read-write and
    // designed for periodic full-table replacement by background refresh
    // threads. Today it stores TFRs only; NOTAMs / weather / etc. will
    // join as additional source groups.
    //
    // Schema is versioned per source group via a SCHEMA_VERSIONS table,
    // so bumping the TFR schema does not erase NOTAM data, and vice
    // versa. On open, each known group's on-disk version is compared
    // against the expected version: a mismatch (or missing row) logs a
    // warning, drops that group's tables and its SOURCE_META row, and
    // recreates the current schema. Other groups are untouched.
    //
    // Threading: an internal std::mutex serializes all access, mirroring
    // nasr_database. tfr_source owns its own in-memory snapshot (under a
    // separate shared_mutex) and queries this database only at warm-start
    // load and at the end of each refresh — the render loop never touches
    // SQLite directly.
    class ephemeral_database
    {
        struct impl;
        std::unique_ptr<impl> pimpl;

    public:
        // Opens db_path read-write, creating the file if missing. Runs
        // schema version checks for every known source group and rebuilds
        // any that are missing or stale.
        explicit ephemeral_database(const std::filesystem::path& db_path);
        ~ephemeral_database();

        ephemeral_database(const ephemeral_database&) = delete;
        ephemeral_database& operator=(const ephemeral_database&) = delete;

        // ----- SOURCE_META -----
        // Per-source freshness metadata. `source_name` is the same key
        // surfaced through data_source::name ("tfr", "notam", ...).
        // last_refreshed() returns nullopt when no row exists; etag()
        // returns "" in that case.
        std::optional<std::chrono::system_clock::time_point>
            last_refreshed(const std::string& source_name) const;
        std::string etag(const std::string& source_name) const;
        void set_source_meta(const std::string& source_name,
                             std::chrono::system_clock::time_point refreshed,
                             const std::string& etag);

        // ----- TFR -----
        // Read every TFR back into memory. Used by tfr_source on warm
        // start; returns owning copies. Empty vector on a fresh database.
        std::vector<tfr> load_tfrs() const;

        // Atomically replace the TFR table contents inside one transaction.
        // tfr_id / tfr_area::area_id values from the input are written
        // as given — tfr_source assigns sequential ids before calling.
        void replace_tfrs(const std::vector<tfr>& tfrs);
    };
}
