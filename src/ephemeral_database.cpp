#include "ephemeral_database.hpp"
#include <chrono>
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
#include <unordered_map>
#include <vector>

namespace osect
{
    namespace
    {
        // Bootstrap tables — exist regardless of which source groups are
        // present. SCHEMA_VERSIONS tracks each group's on-disk version so
        // a stale TFR schema can be rebuilt without touching unrelated
        // groups (NOTAMs, weather, ...) that will eventually live here.
        constexpr const char* BOOTSTRAP_SQL = R"(
            CREATE TABLE IF NOT EXISTS SCHEMA_VERSIONS (
                group_name TEXT PRIMARY KEY,
                version    INTEGER NOT NULL
            );
            CREATE TABLE IF NOT EXISTS SOURCE_META (
                name           TEXT PRIMARY KEY,
                last_refreshed TEXT NOT NULL,
                etag           TEXT NOT NULL DEFAULT ''
            );
        )";

        // TFR group schema. `mod_abs_time` is reserved for the future
        // conditional-refresh work in tfr_source — stays '' until then.
        //
        // TFR_AREA's primary key is composite (tfr_id, area_id) because
        // the parser numbers areas per-TFR (1, 2, 3 within each TFR),
        // not globally — two TFRs each with one area would both carry
        // area_id=1. TFR_AREA_POINT references the same composite.
        constexpr const char* TFR_GROUP_CREATE_SQL = R"(
            CREATE TABLE TFR (
                tfr_id         INTEGER PRIMARY KEY,
                notam_id       TEXT NOT NULL,
                mod_abs_time   TEXT NOT NULL DEFAULT '',
                tfr_type       TEXT NOT NULL,
                facility       TEXT NOT NULL DEFAULT '',
                date_effective TEXT NOT NULL DEFAULT '',
                date_expire    TEXT NOT NULL DEFAULT '',
                description    TEXT NOT NULL DEFAULT ''
            );
            CREATE INDEX IDX_TFR_NOTAM_ID ON TFR(notam_id);

            CREATE TABLE TFR_AREA (
                tfr_id         INTEGER NOT NULL REFERENCES TFR(tfr_id) ON DELETE CASCADE,
                area_id        INTEGER NOT NULL,
                area_seq       INTEGER NOT NULL,
                area_name      TEXT NOT NULL DEFAULT '',
                upper_ft_val   INTEGER NOT NULL DEFAULT 0,
                upper_ft_ref   TEXT NOT NULL DEFAULT '',
                lower_ft_val   INTEGER NOT NULL DEFAULT 0,
                lower_ft_ref   TEXT NOT NULL DEFAULT '',
                date_effective TEXT NOT NULL DEFAULT '',
                date_expire    TEXT NOT NULL DEFAULT '',
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

        constexpr int TFR_GROUP_VERSION = 2;
        constexpr const char* TFR_GROUP_NAME = "tfr";

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
            int Y = 0, M = 0, D = 0, h = 0, m = 0, sec = 0;
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
        // version doesn't match TFR_GROUP_VERSION.
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
                ins_ver.bind(2, TFR_GROUP_VERSION);
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
                    const auto on_disk = check.column_int(0);
                    if(on_disk == TFR_GROUP_VERSION)
                    {
                        needs_rebuild = false;
                    }
                    else
                    {
                        sdl::log_warn(std::string("ephemeral.db: ") + TFR_GROUP_NAME +
                                      " schema version " + std::to_string(on_disk) +
                                      " != " + std::to_string(TFR_GROUP_VERSION) +
                                      "; dropping and rebuilding");
                    }
                }
            }
            if(needs_rebuild)
            {
                rebuild_tfr_group(db);
            }
        }

        // Open the file read-write, set per-connection PRAGMAs, run the
        // bootstrap schema, and ensure each known group is current.
        // Pulled out so it can run inside the impl's member-init list
        // before the prepared statements that depend on the schema.
        sqlite::database open_and_init_schema(const std::filesystem::path& p)
        {
            sqlite::database db(p.string().c_str(), /*read_only=*/false);
            // FK CASCADE is per-connection in SQLite — must be set every open.
            db.exec("PRAGMA foreign_keys = ON");
            db.exec(BOOTSTRAP_SQL);
            ensure_groups(db);
            return db;
        }
    }

    struct ephemeral_database::impl
    {
        sqlite::database db;
        mutable std::mutex mutex;

        sqlite::statement stmt_select_source_meta;
        sqlite::statement stmt_upsert_source_meta;

        sqlite::statement stmt_load_tfrs;
        sqlite::statement stmt_load_areas;
        sqlite::statement stmt_load_points;

        sqlite::statement stmt_insert_tfr;
        sqlite::statement stmt_insert_area;
        sqlite::statement stmt_insert_point;

        explicit impl(const std::filesystem::path& p)
            : db(open_and_init_schema(p))

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
              stmt_load_tfrs(db.prepare(R"(
                SELECT tfr_id, notam_id, mod_abs_time, tfr_type, facility,
                       date_effective, date_expire, description
                FROM TFR
                ORDER BY tfr_id
            )"))

              ,
              stmt_load_areas(db.prepare(R"(
                SELECT tfr_id, area_id, area_name,
                       upper_ft_val, upper_ft_ref, lower_ft_val, lower_ft_ref,
                       date_effective, date_expire
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
                INSERT INTO TFR (tfr_id, notam_id, mod_abs_time, tfr_type, facility,
                                 date_effective, date_expire, description)
                VALUES (?, ?, ?, ?, ?, ?, ?, ?)
            )"))

              ,
              stmt_insert_area(db.prepare(R"(
                INSERT INTO TFR_AREA (tfr_id, area_id, area_seq, area_name,
                                      upper_ft_val, upper_ft_ref, lower_ft_val, lower_ft_ref,
                                      date_effective, date_expire)
                VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
            )"))

              ,
              stmt_insert_point(db.prepare(R"(
                INSERT INTO TFR_AREA_POINT (tfr_id, area_id, seq, lat, lon) VALUES (?, ?, ?, ?, ?)
            )"))
        {
        }
    };

    ephemeral_database::ephemeral_database(const std::filesystem::path& db_path)
        : pimpl(std::make_unique<impl>(db_path))
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
            return std::string();
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

    std::vector<tfr> ephemeral_database::load_tfrs() const
    {
        std::lock_guard<std::mutex> lock(pimpl->mutex);
        std::vector<tfr> tfrs;
        std::unordered_map<int, std::size_t> tfr_idx;

        auto& st = pimpl->stmt_load_tfrs;
        st.reset();
        while(st.step())
        {
            tfr t{};
            t.tfr_id         = st.column_int(0);
            t.notam_id       = st.column_text(1);
            // column 2 (mod_abs_time) is reserved for a future caller;
            // not surfaced through `struct tfr` yet, so skip.
            t.tfr_type       = st.column_text(3);
            t.facility       = st.column_text(4);
            t.date_effective = st.column_text(5);
            t.date_expire    = st.column_text(6);
            t.description    = st.column_text(7);
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
            a.area_id        = sa.column_int(1);
            a.area_name      = sa.column_text(2);
            a.upper_ft_val   = sa.column_int(3);
            a.upper_ft_ref   = sa.column_text(4);
            a.lower_ft_val   = sa.column_int(5);
            a.lower_ft_ref   = sa.column_text(6);
            a.date_effective = sa.column_text(7);
            a.date_expire    = sa.column_text(8);

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
                s.bind(1, t.tfr_id);
                s.bind(2, t.notam_id);
                s.bind(3, std::string{}); // mod_abs_time — reserved
                s.bind(4, t.tfr_type);
                s.bind(5, t.facility);
                s.bind(6, t.date_effective);
                s.bind(7, t.date_expire);
                s.bind(8, t.description);
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
}
