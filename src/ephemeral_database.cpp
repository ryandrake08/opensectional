#include "ephemeral_database.hpp"
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <locale>
#include <map>
#include <mutex>
#include <sdl/log.hpp>
#include <sqlite/database.hpp>
#include <sqlite/statement.hpp>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

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

        // Per-platform app cache directory. `ephemeral.db` sits at
        // the root of this directory — no further subdirectory,
        // since the database is the only file the app caches there.
        //   macOS:   $HOME/Library/Caches/<bundle_id>/
        //   Linux:   ${XDG_CACHE_HOME:-$HOME/.cache}/osect/
        //   Windows: %LOCALAPPDATA%/osect/
        // Created if missing; throws if the platform env var isn't
        // set or the directory can't be created.
        std::filesystem::path app_cache_dir()
        {
#if defined(__APPLE__)
            const auto dir = std::filesystem::path(getenv_or_throw("HOME")) / "Library/Caches" /
                             OSECT_BUNDLE_IDENTIFIER;
#elif defined(_WIN32)
            const auto dir = std::filesystem::path(getenv_or_throw("LOCALAPPDATA")) / "osect";
#else
            const char* xdg = std::getenv("XDG_CACHE_HOME");
            const auto dir = ((xdg && *xdg) ? std::filesystem::path(xdg)
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

        // Bootstrap tables — exist regardless of which source groups are
        // present. SCHEMA_VERSIONS tracks each group's on-disk schema
        // identity (a hash of the canonical CREATE SQL) so a stale TFR
        // schema can be rebuilt without touching unrelated groups
        // (NOTAMs, weather, ...) that will eventually live here.
        //
        // The `version` column stores text — the hex hash from
        // `schema_hash()` below, prefixed with 'h' to keep SQLite's
        // numeric-affinity conversion from re-typing all-digit hashes
        // to INTEGER. Existing installs predating this change have an
        // integer-affinity column declaration; SQLite stores TEXT in
        // it just fine via dynamic typing.
        constexpr const char* BOOTSTRAP_SQL = R"(
            CREATE TABLE IF NOT EXISTS SCHEMA_VERSIONS (
                group_name TEXT PRIMARY KEY,
                version    TEXT NOT NULL
            );
            CREATE TABLE IF NOT EXISTS SOURCE_META (
                name           TEXT PRIMARY KEY,
                last_refreshed TEXT NOT NULL,
                etag           TEXT NOT NULL DEFAULT ''
            );
        )";

        // Whitespace-collapsing normalizer for the canonical CREATE
        // SQL. Multiple runs of any ASCII whitespace become a single
        // space; leading and trailing whitespace are trimmed. SQL
        // comments are not stripped — adding one to TFR_GROUP_CREATE_SQL
        // changes the hash and triggers a rebuild on next open, which
        // is fine for ephemeral data.
        std::string normalize_sql(std::string_view s)
        {
            std::string out;
            out.reserve(s.size());
            bool prev_ws = true; // trim leading
            for(char c : s)
            {
                const bool is_ws = (c == ' ' || c == '\t' || c == '\n' || c == '\r');
                if(is_ws)
                {
                    if(!prev_ws)
                    {
                        out.push_back(' ');
                        prev_ws = true;
                    }
                }
                else
                {
                    out.push_back(c);
                    prev_ws = false;
                }
            }
            if(!out.empty() && out.back() == ' ')
            {
                out.pop_back();
            }
            return out;
        }

        // FNV-1a 64-bit — deterministic across platforms and runs.
        // Sufficient for change-detection on a small canonical string;
        // we don't need cryptographic strength.
        std::uint64_t fnv1a_64(std::string_view s)
        {
            std::uint64_t h = 14695981039346656037ULL;
            for(unsigned char c : s)
            {
                h ^= c;
                h *= 1099511628211ULL;
            }
            return h;
        }

        // Identity hash of a schema group's canonical CREATE SQL,
        // formatted as 'h' + 16 lowercase hex chars (17 total). The
        // 'h' prefix is load-bearing — without it an all-digit hex
        // hash would get stored as INTEGER by SQLite's type affinity.
        std::string schema_hash(std::string_view sql)
        {
            const auto h = fnv1a_64(normalize_sql(sql));
            std::array<char, 18> buf{};
            std::snprintf(buf.data(), buf.size(), "h%016llx", static_cast<unsigned long long>(h));
            return buf.data();
        }

        // TFR group schema.
        //
        // TFR_AREA's primary key is composite (tfr_id, area_id) because
        // the parser numbers areas per-TFR (1, 2, 3 within each TFR),
        // not globally — two TFRs each with one area would both carry
        // area_id=1. TFR_AREA_POINT references the same composite.
        constexpr const char* TFR_GROUP_CREATE_SQL = R"(
            CREATE TABLE TFR (
                tfr_id              INTEGER PRIMARY KEY,
                notam_id            TEXT NOT NULL,
                tfr_type            TEXT NOT NULL,
                facility            TEXT NOT NULL DEFAULT '',
                date_effective      TEXT NOT NULL DEFAULT '',
                date_expire         TEXT NOT NULL DEFAULT '',
                description         TEXT NOT NULL DEFAULT '',
                date_issued         TEXT NOT NULL DEFAULT '',
                city                TEXT NOT NULL DEFAULT '',
                state               TEXT NOT NULL DEFAULT '',
                coord_facility      TEXT NOT NULL DEFAULT '',
                coord_facility_name TEXT NOT NULL DEFAULT '',
                coord_facility_type TEXT NOT NULL DEFAULT '',
                coord_phone         TEXT NOT NULL DEFAULT '',
                coord_freq          TEXT NOT NULL DEFAULT '',
                poc_name            TEXT NOT NULL DEFAULT '',
                poc_org             TEXT NOT NULL DEFAULT '',
                poc_phone           TEXT NOT NULL DEFAULT '',
                poc_freq            TEXT NOT NULL DEFAULT '',
                time_zone           TEXT NOT NULL DEFAULT '',
                expire_time_zone    TEXT NOT NULL DEFAULT ''
            );
            CREATE INDEX IDX_TFR_NOTAM_ID ON TFR(notam_id);

            CREATE TABLE TFR_AREA (
                tfr_id           INTEGER NOT NULL REFERENCES TFR(tfr_id) ON DELETE CASCADE,
                area_id          INTEGER NOT NULL,
                area_seq         INTEGER NOT NULL,
                area_name        TEXT NOT NULL DEFAULT '',
                upper_ft_val     INTEGER NOT NULL DEFAULT 0,
                upper_ft_ref     TEXT NOT NULL DEFAULT '',
                lower_ft_val     INTEGER NOT NULL DEFAULT 0,
                lower_ft_ref     TEXT NOT NULL DEFAULT '',
                date_effective   TEXT NOT NULL DEFAULT '',
                date_expire      TEXT NOT NULL DEFAULT '',
                start_time       TEXT NOT NULL DEFAULT '',
                end_time         TEXT NOT NULL DEFAULT '',
                is_time_separate TEXT NOT NULL DEFAULT '',
                day_code         TEXT NOT NULL DEFAULT '',
                instructions     TEXT NOT NULL DEFAULT '',
                PRIMARY KEY (tfr_id, area_id)
            ) WITHOUT ROWID;

            CREATE TABLE TFR_AREA_POINT (
                tfr_id  INTEGER NOT NULL,
                area_id INTEGER NOT NULL,
                seq     INTEGER NOT NULL,
                lat     REAL NOT NULL,
                lon     REAL NOT NULL,
                PRIMARY KEY (tfr_id, area_id, seq),
                FOREIGN KEY (tfr_id, area_id) REFERENCES TFR_AREA(tfr_id, area_id) ON DELETE CASCADE
            ) WITHOUT ROWID;
        )";

        constexpr const char* TFR_GROUP_DROP_SQL =
            "DROP TABLE IF EXISTS TFR_AREA_POINT;"
            "DROP TABLE IF EXISTS TFR_AREA;"
            "DROP TABLE IF EXISTS TFR;";

        constexpr const char* TFR_GROUP_NAME = "tfr";

        // Computed once at program startup. Any edit to
        // TFR_GROUP_CREATE_SQL (other than pure whitespace) changes
        // this value; ensure_groups() then sees a mismatch on next
        // open and drops/recreates the TFR tables.
        const std::string TFR_GROUP_SCHEMA_HASH = schema_hash(TFR_GROUP_CREATE_SQL);

        // Strict 20-char UTC ISO 8601: YYYY-MM-DDTHH:MM:SSZ. The
        // ephemeral.db is host-private so we control both writer and
        // reader; no need for the flexible parser nasr_database uses
        // for Python-ingested timestamps.
        std::string format_iso8601(std::chrono::system_clock::time_point tp)
        {
            const auto t = std::chrono::system_clock::to_time_t(tp);
            std::tm tm{};
#if defined(_WIN32)
            gmtime_s(&tm, &t);
#else
            gmtime_r(&t, &tm);
#endif
            std::ostringstream os;
            os.imbue(std::locale::classic());
            os << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
            return os.str();
        }

        std::chrono::system_clock::time_point parse_iso8601(const std::string& s)
        {
            int Y = 0;
            int M = 0;
            int D = 0;
            int h = 0;
            int m = 0;
            int sec = 0;
            if(s.size() != 20 ||
               std::sscanf(s.c_str(), "%4d-%2d-%2dT%2d:%2d:%2dZ", &Y, &M, &D, &h, &m, &sec) != 6)
            {
                throw std::runtime_error("ephemeral.db: malformed timestamp '" + s + "'");
            }
            std::tm tm{};
            tm.tm_year = Y - 1900;
            tm.tm_mon = M - 1;
            tm.tm_mday = D;
            tm.tm_hour = h;
            tm.tm_min = m;
            tm.tm_sec = sec;
#if defined(_WIN32)
            const auto t = _mkgmtime(&tm);
#else
            const auto t = timegm(&tm);
#endif
            return std::chrono::system_clock::from_time_t(t);
        }

        // Drop the TFR group's tables and its SOURCE_META row, then
        // recreate the schema and stamp the version. Used both when the
        // group is missing entirely (fresh file) and when the on-disk
        // schema doesn't match.
        void rebuild_tfr_group(sqlite::database& db)
        {
            db.exec("BEGIN");
            try
            {
                db.exec(TFR_GROUP_DROP_SQL);

                auto del_meta = db.prepare("DELETE FROM SOURCE_META WHERE name = ?");
                del_meta.bind(1, TFR_GROUP_NAME);
                del_meta.step();

                auto del_ver = db.prepare("DELETE FROM SCHEMA_VERSIONS WHERE group_name = ?");
                del_ver.bind(1, TFR_GROUP_NAME);
                del_ver.step();

                db.exec(TFR_GROUP_CREATE_SQL);

                auto ins_ver = db.prepare("INSERT INTO SCHEMA_VERSIONS (group_name, version) VALUES (?, ?)");
                ins_ver.bind(1, TFR_GROUP_NAME);
                ins_ver.bind(2, TFR_GROUP_SCHEMA_HASH);
                ins_ver.step();

                db.exec("COMMIT");
            }
            catch(...)
            {
                try { db.exec("ROLLBACK"); } catch(...) {}
                throw;
            }
        }

        // Ensure every known group's schema is up to date. Called once
        // at construction. Mismatches log a warning and trigger a
        // rebuild; missing groups are created silently.
        void ensure_groups(sqlite::database& db)
        {
            // Read the on-disk version in its own scope so the SELECT
            // cursor finalizes before any rebuild — otherwise the
            // BEGIN/DROP TABLE inside rebuild_tfr_group fails with
            // SQLITE_LOCKED on the still-open SCHEMA_VERSIONS cursor.
            bool needs_rebuild = true;
            {
                auto check = db.prepare("SELECT version FROM SCHEMA_VERSIONS WHERE group_name = ?");
                check.bind(1, TFR_GROUP_NAME);
                if(check.step())
                {
                    const auto on_disk = check.column_text(0);
                    if(on_disk == TFR_GROUP_SCHEMA_HASH)
                    {
                        needs_rebuild = false;
                    }
                    else
                    {
                        sdl::log_warn(std::string("ephemeral.db: ") + TFR_GROUP_NAME +
                                      " schema hash " + on_disk + " != " +
                                      TFR_GROUP_SCHEMA_HASH + "; dropping and rebuilding");
                    }
                }
            }
            if(needs_rebuild)
            {
                rebuild_tfr_group(db);
            }
        }

        // Open the file, and on a read-write open set per-connection
        // PRAGMAs, run the bootstrap schema, and ensure each known group
        // is current. Pulled out so it can run inside the impl's
        // member-init list before the prepared statements that depend on
        // the schema. A read-only open skips every pragma and schema
        // step: WAL and foreign_keys only matter for a writer (WAL is
        // persisted in the file anyway), and the CREATE / rebuild SQL
        // would fail SQLITE_READONLY — a read-only consumer relies on a
        // read-write owner having already brought the file current.
        sqlite::database open_and_init_schema(const std::filesystem::path& p, bool read_only)
        {
            sqlite::database db(p.string().c_str(), read_only);
            if(!read_only)
            {
                // WAL lets db readers proceed without blocking on a writer.
                db.exec("PRAGMA journal_mode = WAL");
                // FK CASCADE is per-connection in SQLite — must be set every open.
                db.exec("PRAGMA foreign_keys = ON");
                db.exec(BOOTSTRAP_SQL);
                ensure_groups(db);
            }
            return db;
        }
    }

    struct ephemeral_database::impl
    {
        sqlite::database db;
        mutable std::mutex mutex;

        sqlite::statement stmt_select_source_meta;
        sqlite::statement stmt_upsert_source_meta;

        sqlite::statement stmt_query_tfrs;
        sqlite::statement stmt_load_areas;
        sqlite::statement stmt_load_points;

        sqlite::statement stmt_insert_tfr;
        sqlite::statement stmt_insert_area;
        sqlite::statement stmt_insert_point;

        explicit impl(const std::filesystem::path& p, bool read_only)
            : db(open_and_init_schema(p, read_only))

              ,
              stmt_select_source_meta(db.prepare(R"(
                SELECT last_refreshed, etag FROM SOURCE_META WHERE name = ?
            )"))

              ,
              stmt_upsert_source_meta(db.prepare(R"(
                INSERT INTO SOURCE_META (name, last_refreshed, etag) VALUES (?, ?, ?)
                ON CONFLICT(name) DO UPDATE SET last_refreshed = excluded.last_refreshed,
                                                etag           = excluded.etag
            )"))

              ,
              stmt_query_tfrs(db.prepare(R"(
                SELECT tfr_id, notam_id, tfr_type, facility,
                       date_effective, date_expire, description,
                       date_issued, city, state,
                       coord_facility, coord_facility_name, coord_facility_type,
                       coord_phone, coord_freq,
                       poc_name, poc_org, poc_phone, poc_freq,
                       time_zone, expire_time_zone
                FROM TFR
                ORDER BY tfr_id
            )"))

              ,
              stmt_load_areas(db.prepare(R"(
                SELECT tfr_id, area_id, area_name,
                       upper_ft_val, upper_ft_ref, lower_ft_val, lower_ft_ref,
                       date_effective, date_expire,
                       start_time, end_time, is_time_separate, day_code,
                       instructions
                FROM TFR_AREA
                ORDER BY tfr_id, area_seq
            )"))

              ,
              stmt_load_points(db.prepare(R"(
                SELECT tfr_id, area_id, lat, lon
                FROM TFR_AREA_POINT
                ORDER BY tfr_id, area_id, seq
            )"))

              ,
              stmt_insert_tfr(db.prepare(R"(
                INSERT INTO TFR (tfr_id, notam_id, tfr_type, facility,
                                 date_effective, date_expire, description,
                                 date_issued, city, state,
                                 coord_facility, coord_facility_name, coord_facility_type,
                                 coord_phone, coord_freq,
                                 poc_name, poc_org, poc_phone, poc_freq,
                                 time_zone, expire_time_zone)
                VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
            )"))

              ,
              stmt_insert_area(db.prepare(R"(
                INSERT INTO TFR_AREA (tfr_id, area_id, area_seq, area_name,
                                      upper_ft_val, upper_ft_ref, lower_ft_val, lower_ft_ref,
                                      date_effective, date_expire,
                                      start_time, end_time, is_time_separate, day_code,
                                      instructions)
                VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
            )"))

              ,
              stmt_insert_point(db.prepare(R"(
                INSERT INTO TFR_AREA_POINT (tfr_id, area_id, seq, lat, lon) VALUES (?, ?, ?, ?, ?)
            )"))
        {
        }
    };

    std::filesystem::path ephemeral_database::default_path()
    {
        return app_cache_dir() / "ephemeral.db";
    }

    ephemeral_database::ephemeral_database(const std::filesystem::path& db_path, bool read_only)
        : pimpl(std::make_unique<impl>(db_path, read_only))
    {
    }

    ephemeral_database::~ephemeral_database() = default;

    std::optional<std::chrono::system_clock::time_point>
    ephemeral_database::last_refreshed(const std::string& source_name) const
    {
        std::lock_guard<std::mutex> lock(pimpl->mutex);
        auto& stmt = pimpl->stmt_select_source_meta;
        stmt.reset();
        stmt.bind(1, source_name);
        if(!stmt.step())
        {
            return std::nullopt;
        }
        return parse_iso8601(stmt.column_text(0));
    }

    std::string ephemeral_database::etag(const std::string& source_name) const
    {
        std::lock_guard<std::mutex> lock(pimpl->mutex);
        auto& stmt = pimpl->stmt_select_source_meta;
        stmt.reset();
        stmt.bind(1, source_name);
        if(!stmt.step())
        {
            return {};
        }
        return stmt.column_text(1);
    }

    void ephemeral_database::set_source_meta(const std::string& source_name,
                                             std::chrono::system_clock::time_point refreshed,
                                             const std::string& etag)
    {
        std::lock_guard<std::mutex> lock(pimpl->mutex);
        auto& stmt = pimpl->stmt_upsert_source_meta;
        stmt.reset();
        stmt.bind(1, source_name);
        stmt.bind(2, format_iso8601(refreshed));
        stmt.bind(3, etag);
        stmt.step();
    }

    std::vector<tfr> ephemeral_database::query_tfrs() const
    {
        std::lock_guard<std::mutex> lock(pimpl->mutex);
        std::vector<tfr> tfrs;
        std::unordered_map<int, std::size_t> tfr_idx;

        auto& st = pimpl->stmt_query_tfrs;
        st.reset();
        while(st.step())
        {
            tfr t{};
            t.tfr_id              = st.column_int(0);
            t.notam_id            = st.column_text(1);
            t.tfr_type            = st.column_text(2);
            t.facility            = st.column_text(3);
            t.date_effective      = st.column_text(4);
            t.date_expire         = st.column_text(5);
            t.description         = st.column_text(6);
            t.date_issued         = st.column_text(7);
            t.city                = st.column_text(8);
            t.state               = st.column_text(9);
            t.coord_facility      = st.column_text(10);
            t.coord_facility_name = st.column_text(11);
            t.coord_facility_type = st.column_text(12);
            t.coord_phone         = st.column_text(13);
            t.coord_freq          = st.column_text(14);
            t.poc_name            = st.column_text(15);
            t.poc_org             = st.column_text(16);
            t.poc_phone           = st.column_text(17);
            t.poc_freq            = st.column_text(18);
            t.time_zone           = st.column_text(19);
            t.expire_time_zone    = st.column_text(20);
            tfr_idx.emplace(t.tfr_id, tfrs.size());
            tfrs.push_back(std::move(t));
        }

        // area_id is per-TFR (not globally unique), so the lookup key
        // is the composite (tfr_id, area_id).
        std::map<std::pair<int, int>, std::pair<std::size_t, std::size_t>> area_idx;

        auto& sa = pimpl->stmt_load_areas;
        sa.reset();
        while(sa.step())
        {
            const int tfr_id = sa.column_int(0);
            tfr_area a{};
            a.area_id          = sa.column_int(1);
            a.area_name        = sa.column_text(2);
            a.upper_ft_val     = sa.column_int(3);
            a.upper_ft_ref     = sa.column_text(4);
            a.lower_ft_val     = sa.column_int(5);
            a.lower_ft_ref     = sa.column_text(6);
            a.date_effective   = sa.column_text(7);
            a.date_expire      = sa.column_text(8);
            a.start_time       = sa.column_text(9);
            a.end_time         = sa.column_text(10);
            a.is_time_separate = sa.column_text(11);
            a.day_code         = sa.column_text(12);
            a.instructions     = sa.column_text(13);

            // FK CASCADE prevents orphan rows.
            const auto parent_pos = tfr_idx.at(tfr_id);
            auto& parent          = tfrs[parent_pos];
            area_idx.emplace(std::make_pair(tfr_id, a.area_id),
                             std::make_pair(parent_pos, parent.areas.size()));
            parent.areas.push_back(std::move(a));
        }

        auto& sp = pimpl->stmt_load_points;
        sp.reset();
        while(sp.step())
        {
            const int tfr_id  = sp.column_int(0);
            const int area_id = sp.column_int(1);
            airspace_point p{};
            p.lat             = sp.column_double(2);
            p.lon             = sp.column_double(3);
            const auto loc    = area_idx.at(std::make_pair(tfr_id, area_id));
            tfrs[loc.first].areas[loc.second].points.push_back(p);
        }

        return tfrs;
    }

    void ephemeral_database::replace_tfrs(const std::vector<tfr>& tfrs)
    {
        std::lock_guard<std::mutex> lock(pimpl->mutex);
        pimpl->db.exec("BEGIN");
        try
        {
            // CASCADE propagates to TFR_AREA and TFR_AREA_POINT.
            pimpl->db.exec("DELETE FROM TFR");

            for(const auto& t : tfrs)
            {
                auto& s = pimpl->stmt_insert_tfr;
                s.reset();
                s.bind(1,  t.tfr_id);
                s.bind(2,  t.notam_id);
                s.bind(3,  t.tfr_type);
                s.bind(4,  t.facility);
                s.bind(5,  t.date_effective);
                s.bind(6,  t.date_expire);
                s.bind(7,  t.description);
                s.bind(8,  t.date_issued);
                s.bind(9,  t.city);
                s.bind(10, t.state);
                s.bind(11, t.coord_facility);
                s.bind(12, t.coord_facility_name);
                s.bind(13, t.coord_facility_type);
                s.bind(14, t.coord_phone);
                s.bind(15, t.coord_freq);
                s.bind(16, t.poc_name);
                s.bind(17, t.poc_org);
                s.bind(18, t.poc_phone);
                s.bind(19, t.poc_freq);
                s.bind(20, t.time_zone);
                s.bind(21, t.expire_time_zone);
                s.step();

                int area_seq = 0;
                for(const auto& a : t.areas)
                {
                    auto& sa = pimpl->stmt_insert_area;
                    sa.reset();
                    sa.bind(1, t.tfr_id);
                    sa.bind(2, a.area_id);
                    sa.bind(3, area_seq++);
                    sa.bind(4, a.area_name);
                    sa.bind(5, a.upper_ft_val);
                    sa.bind(6, a.upper_ft_ref);
                    sa.bind(7, a.lower_ft_val);
                    sa.bind(8, a.lower_ft_ref);
                    sa.bind(9, a.date_effective);
                    sa.bind(10, a.date_expire);
                    sa.bind(11, a.start_time);
                    sa.bind(12, a.end_time);
                    sa.bind(13, a.is_time_separate);
                    sa.bind(14, a.day_code);
                    sa.bind(15, a.instructions);
                    sa.step();

                    int point_seq = 0;
                    for(const auto& p : a.points)
                    {
                        auto& sp = pimpl->stmt_insert_point;
                        sp.reset();
                        sp.bind(1, t.tfr_id);
                        sp.bind(2, a.area_id);
                        sp.bind(3, point_seq++);
                        sp.bind(4, p.lat);
                        sp.bind(5, p.lon);
                        sp.step();
                    }
                }
            }

            pimpl->db.exec("COMMIT");
        }
        catch(...)
        {
            try { pimpl->db.exec("ROLLBACK"); } catch(...) {}
            throw;
        }
    }

    namespace
    {
        // Per-source display config. Add a row when a new source group lands.
        struct source_display
        {
            const char* name;
            const char* prefix; // "TFR ", "NOTAM ", ...
            std::chrono::hours expires_after;
        };
        constexpr std::array<source_display, 1> SOURCE_DISPLAY = {{
            {"tfr", "TFR ", std::chrono::hours(24)},
        }};

        std::string format_info(const source_display& d, std::chrono::system_clock::time_point t)
        {
            const auto tt = std::chrono::system_clock::to_time_t(t);
            std::tm tm{};
#if defined(_WIN32)
            gmtime_s(&tm, &tt);
#else
            gmtime_r(&tt, &tm);
#endif
            // Pin to the "C" locale so month abbreviations are always
            // English ("Apr", not "avr." / "4月" / etc.), matching the
            // static (nasr) sources' info strings.
            std::ostringstream oss;
            oss.imbue(std::locale::classic());
            oss << d.prefix << std::put_time(&tm, "%d %b %Y");
            return oss.str();
        }
    }

    std::vector<data_source> ephemeral_database::list_data_sources() const
    {
        std::vector<data_source> out;
        out.reserve(std::size(SOURCE_DISPLAY));
        for(const auto& d : SOURCE_DISPLAY)
        {
            data_source s;
            s.name = d.name;
            const auto last = last_refreshed(d.name);
            if(last)
            {
                s.expires = *last + d.expires_after;
                s.info = format_info(d, *last);
            }
            else
            {
                s.info = std::string(d.prefix) + "(no data yet)";
            }
            out.push_back(std::move(s));
        }
        return out;
    }
}
