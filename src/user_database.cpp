#include "user_database.hpp"
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <locale>
#include <mutex>
#include <sqlite/database.hpp>
#include <sqlite/statement.hpp>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>

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

        // The durable per-platform application data directory.
        // Distinct from ephemeral_database's cache path — that
        // one points at the OS cache directory, which is purgeable
        // by contract and unsuitable for user-authored data.
        //   macOS:   $HOME/Library/Application Support/<bundle_id>/
        //   Linux:   ${XDG_DATA_HOME:-$HOME/.local/share}/osect/
        //   Windows: %APPDATA%/osect/  (Roaming)
        std::filesystem::path app_user_data_dir()
        {
#if defined(__APPLE__)
            const auto dir = std::filesystem::path(getenv_or_throw("HOME")) /
                             "Library/Application Support" / OSECT_BUNDLE_IDENTIFIER;
#elif defined(_WIN32)
            const auto dir = std::filesystem::path(getenv_or_throw("APPDATA")) / "osect";
#else
            const char* xdg = std::getenv("XDG_DATA_HOME");
            const auto dir = ((xdg && *xdg) ? std::filesystem::path(xdg)
                                            : std::filesystem::path(getenv_or_throw("HOME")) / ".local/share") /
                             "osect";
#endif
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            if(ec)
            {
                throw std::runtime_error("failed to create user data dir '" + dir.string() + "': " + ec.message());
            }
            return dir;
        }

        constexpr const char* BOOTSTRAP_SQL = R"(
            CREATE TABLE IF NOT EXISTS SCHEMA_VERSIONS (
                group_name TEXT PRIMARY KEY,
                version    INTEGER NOT NULL
            );
        )";

        // Routes group, v2.
        //
        // route_id is AUTOINCREMENT so rowids are strictly monotonic
        // and never reused — future cross-references can rely on a
        // row's identity remaining stable after surrounding rows are
        // deleted.
        //
        // ROUTE_WAYPOINT holds the route in resolved form: one row per
        // waypoint, in route order (`seq`), carrying its coordinates so
        // a flight_route can be rebuilt with no nasr_database. Rows
        // sharing an `element_index` whose `airway_id` is non-empty
        // form one airway traversal; an empty `airway_id` marks a
        // standalone waypoint. ON DELETE CASCADE drops a route's
        // waypoints with the route.
        constexpr int ROUTES_GROUP_VERSION = 2;
        constexpr const char* ROUTES_GROUP_NAME = "routes";
        constexpr const char* ROUTES_GROUP_DROP_SQL = R"(
            DROP TABLE IF EXISTS ROUTE_WAYPOINT;
            DROP TABLE IF EXISTS ROUTE;
        )";
        constexpr const char* ROUTES_GROUP_CREATE_SQL = R"(
            CREATE TABLE ROUTE (
                route_id    INTEGER PRIMARY KEY AUTOINCREMENT,
                name        TEXT NOT NULL DEFAULT '',
                created_at  TEXT NOT NULL,
                updated_at  TEXT NOT NULL
            );
            CREATE TABLE ROUTE_WAYPOINT (
                route_id      INTEGER NOT NULL REFERENCES ROUTE(route_id) ON DELETE CASCADE,
                seq           INTEGER NOT NULL,
                element_index INTEGER NOT NULL,
                kind          TEXT NOT NULL,
                identifier    TEXT NOT NULL DEFAULT '',
                lat           REAL NOT NULL,
                lon           REAL NOT NULL,
                airway_id     TEXT NOT NULL DEFAULT '',
                PRIMARY KEY (route_id, seq)
            );
        )";

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

        std::string now_iso8601()
        {
            return format_iso8601(std::chrono::system_clock::now());
        }

        // Bring the routes group to ROUTES_GROUP_VERSION:
        //   missing    → create v2, stamp version.
        //   == current → no-op.
        //   > current  → throw. Refuse to operate on a database newer
        //                than this build understands.
        //   < current  → drop and recreate. No users in the field yet,
        //                so the v1→v2 change carries no migration; a
        //                forward-migration branch goes here later.
        void ensure_routes_group(sqlite::database& db)
        {
            int on_disk = 0;
            bool present = false;
            {
                auto check = db.prepare("SELECT version FROM SCHEMA_VERSIONS WHERE group_name = ?");
                check.bind(1, ROUTES_GROUP_NAME);
                if(check.step())
                {
                    on_disk = check.column_int(0);
                    present = true;
                }
            }

            if(present && on_disk == ROUTES_GROUP_VERSION)
            {
                return;
            }

            if(present && on_disk > ROUTES_GROUP_VERSION)
            {
                throw std::runtime_error(
                    std::string("user.db: '") + ROUTES_GROUP_NAME + "' group is at version " +
                    std::to_string(on_disk) + " but this build only understands version " +
                    std::to_string(ROUTES_GROUP_VERSION) +
                    ". Refusing to open — upgrade the application or restore a backup.");
            }

            db.exec("BEGIN");
            try
            {
                db.exec(ROUTES_GROUP_DROP_SQL);
                db.exec(ROUTES_GROUP_CREATE_SQL);
                if(present)
                {
                    auto upd = db.prepare("UPDATE SCHEMA_VERSIONS SET version = ? WHERE group_name = ?");
                    upd.bind(1, ROUTES_GROUP_VERSION);
                    upd.bind(2, ROUTES_GROUP_NAME);
                    upd.step();
                }
                else
                {
                    auto ins = db.prepare("INSERT INTO SCHEMA_VERSIONS (group_name, version) VALUES (?, ?)");
                    ins.bind(1, ROUTES_GROUP_NAME);
                    ins.bind(2, ROUTES_GROUP_VERSION);
                    ins.step();
                }
                db.exec("COMMIT");
            }
            catch(...)
            {
                try
                {
                    db.exec("ROLLBACK");
                }
                catch(...)
                {
                }
                throw;
            }
        }

        // A read-only open skips every pragma and schema step: WAL and
        // foreign_keys are connection settings that only matter for a
        // writer (WAL is persisted in the file anyway), and CREATE /
        // migration SQL would fail SQLITE_READONLY on the handle. A
        // read-only consumer therefore relies on a read-write owner
        // having already brought the file's schema current.
        sqlite::database open_and_init_schema(const std::filesystem::path& p, bool read_only)
        {
            sqlite::database db(p.string().c_str(), read_only);
            if(!read_only)
            {
                // WAL lets readers proceed without blocking on a writer,
                // matching ephemeral_database — needed once consumers
                // open their own read-only connections.
                db.exec("PRAGMA journal_mode = WAL");
                db.exec("PRAGMA foreign_keys = ON");
                db.exec(BOOTSTRAP_SQL);
                ensure_routes_group(db);
            }
            return db;
        }

        // Read a waypoint row from a stepped statement. `base` is the
        // column index of element_index; the following five columns
        // are kind, identifier, lat, lon, airway_id in that order.
        route_waypoint_row read_waypoint_row(sqlite::statement& st, int base)
        {
            route_waypoint_row r;
            r.element_index = st.column_int(base + 0);
            r.kind          = st.column_text(base + 1);
            r.identifier    = st.column_text(base + 2);
            r.lat           = st.column_double(base + 3);
            r.lon           = st.column_double(base + 4);
            auto airway     = st.column_text(base + 5);
            r.airway_id = airway.empty() ? std::nullopt : std::optional<std::string>(std::move(airway));
            return r;
        }
    }

    struct user_database::impl
    {
        sqlite::database db;
        mutable std::mutex mutex;

        sqlite::statement stmt_load_routes;
        sqlite::statement stmt_load_all_waypoints;
        sqlite::statement stmt_query_route;
        sqlite::statement stmt_query_waypoints;
        sqlite::statement stmt_route_exists;
        sqlite::statement stmt_insert_route;
        sqlite::statement stmt_touch_route;
        sqlite::statement stmt_insert_waypoint;
        sqlite::statement stmt_delete_waypoints;
        sqlite::statement stmt_delete_route;

        explicit impl(const std::filesystem::path& p, bool read_only)
            : db(open_and_init_schema(p, read_only)),
              stmt_load_routes(db.prepare(R"(
                SELECT route_id, name FROM ROUTE ORDER BY route_id
            )")),
              stmt_load_all_waypoints(db.prepare(R"(
                SELECT route_id, element_index, kind, identifier, lat, lon, airway_id
                FROM ROUTE_WAYPOINT ORDER BY route_id, seq
            )")),
              stmt_query_route(db.prepare(R"(
                SELECT route_id, name FROM ROUTE WHERE route_id = ?
            )")),
              stmt_query_waypoints(db.prepare(R"(
                SELECT element_index, kind, identifier, lat, lon, airway_id
                FROM ROUTE_WAYPOINT WHERE route_id = ? ORDER BY seq
            )")),
              stmt_route_exists(db.prepare(R"(
                SELECT 1 FROM ROUTE WHERE route_id = ?
            )")),
              stmt_insert_route(db.prepare(R"(
                INSERT INTO ROUTE (created_at, updated_at) VALUES (?, ?)
            )")),
              stmt_touch_route(db.prepare(R"(
                UPDATE ROUTE SET updated_at = ? WHERE route_id = ?
            )")),
              stmt_insert_waypoint(db.prepare(R"(
                INSERT INTO ROUTE_WAYPOINT
                    (route_id, seq, element_index, kind, identifier, lat, lon, airway_id)
                VALUES (?, ?, ?, ?, ?, ?, ?, ?)
            )")),
              stmt_delete_waypoints(db.prepare(R"(
                DELETE FROM ROUTE_WAYPOINT WHERE route_id = ?
            )")),
              stmt_delete_route(db.prepare(R"(
                DELETE FROM ROUTE WHERE route_id = ?
            )"))
        {
        }

        // Insert every waypoint row for a route, in order. Caller
        // holds the transaction.
        void write_waypoints(std::int64_t route_id, const std::vector<route_waypoint_row>& waypoints)
        {
            auto& s = stmt_insert_waypoint;
            for(std::size_t i = 0; i < waypoints.size(); ++i)
            {
                const auto& w = waypoints[i];
                s.reset();
                s.bind(1, route_id);
                s.bind(2, static_cast<std::int64_t>(i));
                s.bind(3, w.element_index);
                s.bind(4, w.kind);
                s.bind(5, w.identifier);
                s.bind(6, w.lat);
                s.bind(7, w.lon);
                s.bind(8, w.airway_id.value_or(std::string{}));
                s.step();
            }
        }
    };

    std::filesystem::path user_database::default_path()
    {
        return app_user_data_dir() / "user.db";
    }

    user_database::user_database(const std::filesystem::path& db_path, bool read_only)
        : pimpl(std::make_unique<impl>(db_path, read_only))
    {
    }

    user_database::~user_database() = default;

    std::vector<route_record> user_database::load_routes() const
    {
        std::lock_guard<std::mutex> lock(pimpl->mutex);
        std::vector<route_record> out;
        std::unordered_map<std::int64_t, std::size_t> index_by_id;

        auto& routes = pimpl->stmt_load_routes;
        routes.reset();
        while(routes.step())
        {
            route_record rec;
            rec.route_id = routes.column_int64(0);
            rec.name     = routes.column_text(1);
            index_by_id[rec.route_id] = out.size();
            out.push_back(std::move(rec));
        }

        auto& wps = pimpl->stmt_load_all_waypoints;
        wps.reset();
        while(wps.step())
        {
            const auto rid = wps.column_int64(0);
            const auto it = index_by_id.find(rid);
            if(it != index_by_id.end())
            {
                out[it->second].waypoints.push_back(read_waypoint_row(wps, 1));
            }
        }
        return out;
    }

    std::optional<route_record> user_database::query_route(std::int64_t route_id) const
    {
        std::lock_guard<std::mutex> lock(pimpl->mutex);
        route_record rec;
        {
            auto& st = pimpl->stmt_query_route;
            st.reset();
            st.bind(1, route_id);
            if(!st.step())
            {
                return std::nullopt;
            }
            rec.route_id = st.column_int64(0);
            rec.name     = st.column_text(1);
        }
        auto& wps = pimpl->stmt_query_waypoints;
        wps.reset();
        wps.bind(1, route_id);
        while(wps.step())
        {
            rec.waypoints.push_back(read_waypoint_row(wps, 0));
        }
        return rec;
    }

    std::int64_t user_database::insert_route(const std::vector<route_waypoint_row>& waypoints)
    {
        std::lock_guard<std::mutex> lock(pimpl->mutex);
        const auto ts = now_iso8601();
        pimpl->db.exec("BEGIN");
        try
        {
            auto& s = pimpl->stmt_insert_route;
            s.reset();
            s.bind(1, ts);
            s.bind(2, ts);
            s.step();
            const auto route_id = pimpl->db.last_insert_rowid();
            pimpl->write_waypoints(route_id, waypoints);
            pimpl->db.exec("COMMIT");
            return route_id;
        }
        catch(...)
        {
            try
            {
                pimpl->db.exec("ROLLBACK");
            }
            catch(...)
            {
            }
            throw;
        }
    }

    void user_database::update_route(std::int64_t route_id, const std::vector<route_waypoint_row>& waypoints)
    {
        std::lock_guard<std::mutex> lock(pimpl->mutex);
        pimpl->db.exec("BEGIN");
        try
        {
            bool exists = false;
            {
                auto& s = pimpl->stmt_route_exists;
                s.reset();
                s.bind(1, route_id);
                exists = s.step();
            }
            if(exists)
            {
                auto& touch = pimpl->stmt_touch_route;
                touch.reset();
                touch.bind(1, now_iso8601());
                touch.bind(2, route_id);
                touch.step();

                auto& del = pimpl->stmt_delete_waypoints;
                del.reset();
                del.bind(1, route_id);
                del.step();

                pimpl->write_waypoints(route_id, waypoints);
            }
            pimpl->db.exec("COMMIT");
        }
        catch(...)
        {
            try
            {
                pimpl->db.exec("ROLLBACK");
            }
            catch(...)
            {
            }
            throw;
        }
    }

    void user_database::delete_route(std::int64_t route_id)
    {
        std::lock_guard<std::mutex> lock(pimpl->mutex);
        auto& s = pimpl->stmt_delete_route;
        s.reset();
        s.bind(1, route_id);
        s.step();
    }
}
