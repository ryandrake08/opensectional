#pragma once

#include "data_source.hpp"
#include "geo_types.hpp"
#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace osect
{
    struct tfr_area
    {
        int area_id;
        std::string area_name;
        int upper_ft_val;
        std::string upper_ft_ref; // "MSL" / "SFC" / "AGL" / "STD" / "OTHER"
        int lower_ft_val;
        std::string lower_ft_ref; // "MSL" / "SFC" / "AGL" / "STD" / "OTHER"
        std::string date_effective;
        std::string date_expire;
        std::string start_time;        // daily window start, HHMM
        std::string end_time;          // daily window end, HHMM
        std::string is_time_separate;  // "TRUE" / "FALSE" — daily-recurring flag
        std::string day_code;          // days-of-week mask (FAA encoding)
        std::string instructions; // multiple <txtInstr> joined with '\n'
        std::vector<airspace_point> points;
    };

    struct tfr
    {
        int tfr_id;
        std::string notam_id;
        std::string tfr_type;
        std::string facility;
        std::string date_effective;
        std::string date_expire;
        std::string description;
        std::string date_issued;
        std::string city;
        std::string state;
        std::string coord_facility;
        std::string coord_facility_name;
        std::string coord_facility_type;
        std::string coord_phone;
        std::string coord_freq;
        std::string poc_name;
        std::string poc_org;
        std::string poc_phone;
        std::string poc_freq;
        std::string time_zone;        // e.g. "EDT" — context for date_effective / date_expire
        std::string expire_time_zone; // e.g. "EDT" — context for date_expire when it differs
        std::vector<tfr_area> areas;
    };

    // Pre-tessellated rendering chunk. Built from tfr_area's polygon
    // by subdividing the ring into overlapping fixed-length spans;
    // see `subdivide_ring` (currently in tools/build_common.py and
    // soon to be ported as a small C++ helper).
    struct tfr_segment
    {
        int upper_ft_val;
        std::string upper_ft_ref;
        int lower_ft_val;
        std::string lower_ft_ref;
        std::vector<airspace_point> points;
    };

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
    // nasr_database. tfr_refresher owns its own in-memory snapshot (under a
    // separate shared_mutex) and queries this database only at warm-start
    // load and at the end of each refresh — the render loop never touches
    // SQLite directly.
    class ephemeral_database
    {
        struct impl;
        std::unique_ptr<impl> pimpl;

    public:
        // The per-platform default cache path for ephemeral.db. Used
        // by every consumer (refresher writing, feature_builder /
        // map_widget reading) to open their own connection to the
        // same file. Resolution rules:
        //   macOS:   $HOME/Library/Caches/<bundle_id>/ephemeral.db
        //   Linux:   ${XDG_CACHE_HOME:-$HOME/.cache}/osect/ephemeral.db
        //   Windows: %LOCALAPPDATA%/osect/ephemeral.db
        // Creates the parent directory if missing; throws if the
        // platform env var isn't set or the directory can't be
        // created.
        static std::filesystem::path default_path();

        // Opens db_path. read_only (the default) opens the file with
        // SQLITE_OPEN_READONLY and skips all schema work — the caller
        // must point it at a database a read-write owner already
        // created and migrated. read_only = false creates the file if
        // missing, runs the schema version checks for every known
        // source group, and rebuilds any that are missing or stale.
        // Hold a read-only handle as `const ephemeral_database` so the
        // mutator API is a compile error rather than a runtime
        // SQLITE_READONLY.
        explicit ephemeral_database(const std::filesystem::path& db_path, bool read_only = true);
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
        // Read every TFR currently persisted. The canonical read API
        // for TFR data — every consumer that needs TFRs goes through
        // this (warm-start population, render-path build, pick-path
        // hit test) rather than caching a snapshot elsewhere. Empty
        // vector on a fresh database. Each call hits SQLite (under
        // WAL, concurrent with the refresher's writes); the cost is
        // small (< 100 TFRs nationwide).
        std::vector<tfr> query_tfrs() const;

        // Atomically replace the TFR table contents inside one transaction.
        // tfr_id / tfr_area::area_id values from the input are written
        // as given — the refresher assigns sequential ids before calling.
        void replace_tfrs(const std::vector<tfr>& tfrs);

        // One data_source per ephemeral source this database knows
        // about. Mirrors nasr_database::list_data_sources(); info and
        // expires are formatted from each source's last_refreshed.
        // `updating` is always false — the refresher overlays its own
        // state.
        std::vector<data_source> list_data_sources() const;
    };
}
