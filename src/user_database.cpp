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
#include <unordered_set>

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
        //   Linux:   ${XDG_DATA_HOME:-$HOME/.local/share}/<app_name>/
        //   Windows: %APPDATA%/<app_name>/  (Roaming)
        std::filesystem::path app_user_data_dir()
        {
#if defined(__APPLE__)
            const auto dir = std::filesystem::path(getenv_or_throw("HOME")) / "Library/Application Support" /
                             OSECT_BUNDLE_IDENTIFIER;
#elif defined(_WIN32)
            const auto dir = std::filesystem::path(getenv_or_throw("APPDATA")) / OSECT_APP_NAME;
#else
            const char* xdg = std::getenv("XDG_DATA_HOME");
            const auto dir = ((xdg && *xdg) ? std::filesystem::path(xdg)
                                            : std::filesystem::path(getenv_or_throw("HOME")) / ".local/share") /
                             OSECT_APP_NAME;
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

        // Routes group, v4.
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
        constexpr int ROUTES_GROUP_VERSION = 4;
        constexpr const char* ROUTES_GROUP_NAME = "routes";
        constexpr const char* ROUTES_GROUP_DROP_SQL = R"(
            DROP TABLE IF EXISTS ROUTE_WAYPOINT;
            DROP TABLE IF EXISTS ROUTE;
        )";
        constexpr const char* ROUTES_GROUP_CREATE_SQL = R"(
            CREATE TABLE ROUTE (
                route_id    INTEGER PRIMARY KEY AUTOINCREMENT,
                name        TEXT NOT NULL DEFAULT '',
                cruise_altitude_ft REAL,
                climb_gradient_ft_per_nm REAL NOT NULL,
                descent_gradient_ft_per_nm REAL NOT NULL,
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

        // Waypoints group, v1.
        //
        // USER_WAYPOINT holds user-defined persistent waypoints.
        // waypoint_id is AUTOINCREMENT so a row's identity stays
        // stable for future cross-references. `name` is UNIQUE and,
        // by CHECK, non-empty — it is the identifier the waypoint is
        // referenced by in route shorthand and search.
        constexpr int WAYPOINTS_GROUP_VERSION = 1;
        constexpr const char* WAYPOINTS_GROUP_NAME = "waypoints";
        constexpr const char* WAYPOINTS_GROUP_DROP_SQL = R"(
            DROP TABLE IF EXISTS USER_WAYPOINT;
        )";
        constexpr const char* WAYPOINTS_GROUP_CREATE_SQL = R"(
            CREATE TABLE USER_WAYPOINT (
                waypoint_id INTEGER PRIMARY KEY AUTOINCREMENT,
                name        TEXT NOT NULL UNIQUE CHECK(name <> ''),
                lat         REAL NOT NULL,
                lon         REAL NOT NULL,
                created_at  TEXT NOT NULL,
                updated_at  TEXT NOT NULL
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

        // Bring one schema group to `version`:
        //   missing    → create, stamp version.
        //   == current → no-op.
        //   > current  → throw. Refuse to operate on a database newer
        //                than this build understands.
        //   < current  → drop and recreate. No users in the field yet,
        //                so a schema bump carries no migration; a real
        //                forward-migration branch goes here later.
        void ensure_group(sqlite::database& db, const char* group_name, int version, const char* drop_sql,
                          const char* create_sql)
        {
            int on_disk = 0;
            bool present = false;
            {
                auto check = db.prepare("SELECT version FROM SCHEMA_VERSIONS WHERE group_name = ?");
                check.bind(1, group_name);
                if(check.step())
                {
                    on_disk = check.column_int(0);
                    present = true;
                }
            }

            if(present && on_disk == version)
            {
                return;
            }

            if(present && on_disk > version)
            {
                throw std::runtime_error(std::string("user.db: '") + group_name + "' group is at version " +
                                         std::to_string(on_disk) + " but this build only understands version " +
                                         std::to_string(version) +
                                         ". Refusing to open — upgrade the application or restore a backup.");
            }

            db.exec("BEGIN");
            try
            {
                db.exec(drop_sql);
                db.exec(create_sql);
                if(present)
                {
                    auto upd = db.prepare("UPDATE SCHEMA_VERSIONS SET version = ? WHERE group_name = ?");
                    upd.bind(1, version);
                    upd.bind(2, group_name);
                    upd.step();
                }
                else
                {
                    auto ins = db.prepare("INSERT INTO SCHEMA_VERSIONS (group_name, version) VALUES (?, ?)");
                    ins.bind(1, group_name);
                    ins.bind(2, version);
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

        // Open the file read-write, set per-connection PRAGMAs, run the
        // bootstrap schema, and ensure each known group is current.
        sqlite::database open_and_init_schema(const std::filesystem::path& p)
        {
            sqlite::database db(p.string().c_str(), /*read_only=*/false);
            // WAL lets readers proceed without blocking on a writer.
            db.exec("PRAGMA journal_mode = WAL");
            // FK CASCADE is per-connection in SQLite — must be set every open.
            db.exec("PRAGMA foreign_keys = ON");
            db.exec(BOOTSTRAP_SQL);
            ensure_group(db, ROUTES_GROUP_NAME, ROUTES_GROUP_VERSION, ROUTES_GROUP_DROP_SQL, ROUTES_GROUP_CREATE_SQL);
            ensure_group(db, WAYPOINTS_GROUP_NAME, WAYPOINTS_GROUP_VERSION, WAYPOINTS_GROUP_DROP_SQL,
                         WAYPOINTS_GROUP_CREATE_SQL);
            return db;
        }

        // Read a waypoint row from a stepped statement. `base` is the
        // column index of element_index; the following five columns
        // are kind, identifier, lat, lon, airway_id in that order.
        route_waypoint_row read_waypoint_row(sqlite::statement& st, int base)
        {
            route_waypoint_row r;
            r.element_index = st.column_int(base + 0);
            r.kind = st.column_text(base + 1);
            r.identifier = st.column_text(base + 2);
            r.lat = st.column_double(base + 3);
            r.lon = st.column_double(base + 4);
            auto airway = st.column_text(base + 5);
            r.airway_id = airway.empty() ? std::nullopt : std::optional<std::string>(std::move(airway));
            return r;
        }

        // The integer suffix of an auto-generated waypoint name —
        // "WPT" followed by one or more digits — or nullopt for any
        // other name. Used to pick the next free WPT<n>.
        std::optional<int> wpt_name_number(const std::string& name)
        {
            constexpr std::size_t prefix_len = 3; // "WPT"
            if(name.size() <= prefix_len || name.compare(0, prefix_len, "WPT") != 0)
            {
                return std::nullopt;
            }
            for(std::size_t i = prefix_len; i < name.size(); ++i)
            {
                if(name[i] < '0' || name[i] > '9')
                {
                    return std::nullopt;
                }
            }
            try
            {
                return std::stoi(name.substr(prefix_len));
            }
            catch(const std::out_of_range&)
            {
                return std::nullopt;
            }
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
        sqlite::statement stmt_load_user_waypoints;
        sqlite::statement stmt_query_user_waypoint;
        sqlite::statement stmt_user_waypoint_names;
        sqlite::statement stmt_insert_user_waypoint;
        sqlite::statement stmt_user_waypoint_name_taken;
        sqlite::statement stmt_update_user_waypoint;
        sqlite::statement stmt_delete_user_waypoint;

        explicit impl(const std::filesystem::path& p)
            : db(open_and_init_schema(p)),
              stmt_load_routes(db.prepare(R"(
                SELECT route_id, name, cruise_altitude_ft, climb_gradient_ft_per_nm, descent_gradient_ft_per_nm
                FROM ROUTE ORDER BY route_id
            )")),
              stmt_load_all_waypoints(db.prepare(R"(
                SELECT route_id, element_index, kind, identifier, lat, lon, airway_id
                FROM ROUTE_WAYPOINT ORDER BY route_id, seq
            )")),
              stmt_query_route(db.prepare(R"(
                SELECT route_id, name, cruise_altitude_ft, climb_gradient_ft_per_nm, descent_gradient_ft_per_nm
                FROM ROUTE WHERE route_id = ?
            )")),
              stmt_query_waypoints(db.prepare(R"(
                SELECT element_index, kind, identifier, lat, lon, airway_id
                FROM ROUTE_WAYPOINT WHERE route_id = ? ORDER BY seq
            )")),
              stmt_route_exists(db.prepare(R"(
                SELECT 1 FROM ROUTE WHERE route_id = ?
            )")),
              stmt_insert_route(db.prepare(R"(
                INSERT INTO ROUTE
                    (cruise_altitude_ft, climb_gradient_ft_per_nm, descent_gradient_ft_per_nm, created_at, updated_at)
                VALUES (?, ?, ?, ?, ?)
            )")),
              stmt_touch_route(db.prepare(R"(
                UPDATE ROUTE SET cruise_altitude_ft = ?, updated_at = ? WHERE route_id = ?
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
            )")),
              stmt_load_user_waypoints(db.prepare(R"(
                SELECT waypoint_id, name, lat, lon FROM USER_WAYPOINT ORDER BY waypoint_id
            )")),
              stmt_query_user_waypoint(db.prepare(R"(
                SELECT waypoint_id, name, lat, lon FROM USER_WAYPOINT WHERE waypoint_id = ?
            )")),
              stmt_user_waypoint_names(db.prepare(R"(
                SELECT name FROM USER_WAYPOINT
            )")),
              stmt_insert_user_waypoint(db.prepare(R"(
                INSERT INTO USER_WAYPOINT (name, lat, lon, created_at, updated_at)
                VALUES (?, ?, ?, ?, ?)
            )")),
              stmt_user_waypoint_name_taken(db.prepare(R"(
                SELECT 1 FROM USER_WAYPOINT WHERE name = ? AND waypoint_id != ?
            )")),
              stmt_update_user_waypoint(db.prepare(R"(
                UPDATE USER_WAYPOINT SET name = ?, lat = ?, lon = ?, updated_at = ? WHERE waypoint_id = ?
            )")),
              stmt_delete_user_waypoint(db.prepare(R"(
                DELETE FROM USER_WAYPOINT WHERE waypoint_id = ?
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

    user_database::user_database(const std::filesystem::path& db_path) : pimpl(std::make_unique<impl>(db_path))
    {
    }

    user_database::~user_database() = default;

    std::vector<route_record> user_database::load_routes() const
    {
        std::scoped_lock lock(pimpl->mutex);
        std::vector<route_record> out;
        std::unordered_map<std::int64_t, std::size_t> index_by_id;

        auto& routes = pimpl->stmt_load_routes;
        routes.reset();
        while(routes.step())
        {
            route_record rec;
            rec.route_id = routes.column_int64(0);
            rec.name = routes.column_text(1);
            if(!routes.column_is_null(2))
            {
                rec.cruise_altitude_ft = routes.column_double(2);
            }
            rec.gradients.climb_ft_per_nm = routes.column_double(3);
            rec.gradients.descent_ft_per_nm = routes.column_double(4);
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
        std::scoped_lock lock(pimpl->mutex);
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
            rec.name = st.column_text(1);
            if(!st.column_is_null(2))
            {
                rec.cruise_altitude_ft = st.column_double(2);
            }
            rec.gradients.climb_ft_per_nm = st.column_double(3);
            rec.gradients.descent_ft_per_nm = st.column_double(4);
            // A statement parked on SQLITE_ROW holds an implicit read
            // transaction open until the next reset, pinning this
            // connection to that snapshot. Release it now so concurrent
            // writers' commits become visible on the next query.
            st.reset();
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

    std::int64_t user_database::insert_route(const std::vector<route_waypoint_row>& waypoints,
                                             std::optional<double> cruise_altitude_ft,
                                             terrain_profile_gradients gradients)
    {
        std::scoped_lock lock(pimpl->mutex);
        const auto ts = now_iso8601();
        pimpl->db.exec("BEGIN");
        try
        {
            auto& s = pimpl->stmt_insert_route;
            s.reset();
            if(cruise_altitude_ft)
            {
                s.bind(1, *cruise_altitude_ft);
            }
            else
            {
                s.bind_null(1);
            }
            s.bind(2, gradients.climb_ft_per_nm);
            s.bind(3, gradients.descent_ft_per_nm);
            s.bind(4, ts);
            s.bind(5, ts);
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

    void user_database::update_route(std::int64_t route_id, const std::vector<route_waypoint_row>& waypoints,
                                     std::optional<double> cruise_altitude_ft)
    {
        std::scoped_lock lock(pimpl->mutex);
        pimpl->db.exec("BEGIN");
        try
        {
            bool exists = false;
            {
                auto& s = pimpl->stmt_route_exists;
                s.reset();
                s.bind(1, route_id);
                exists = s.step();
                // Release the implicit read transaction held by a
                // statement parked on SQLITE_ROW — see query_route for
                // the rationale.
                s.reset();
            }
            if(exists)
            {
                auto& touch = pimpl->stmt_touch_route;
                touch.reset();
                if(cruise_altitude_ft)
                {
                    touch.bind(1, *cruise_altitude_ft);
                }
                else
                {
                    touch.bind_null(1);
                }
                touch.bind(2, now_iso8601());
                touch.bind(3, route_id);
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

    void user_database::update_route_cruise_altitude(std::int64_t route_id, std::optional<double> cruise_altitude_ft)
    {
        std::scoped_lock lock(pimpl->mutex);
        pimpl->db.exec("BEGIN");
        try
        {
            auto& update = pimpl->stmt_touch_route;
            update.reset();
            if(cruise_altitude_ft)
            {
                update.bind(1, *cruise_altitude_ft);
            }
            else
            {
                update.bind_null(1);
            }
            update.bind(2, now_iso8601());
            update.bind(3, route_id);
            update.step();
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
        std::scoped_lock lock(pimpl->mutex);
        auto& s = pimpl->stmt_delete_route;
        s.reset();
        s.bind(1, route_id);
        s.step();
    }

    std::vector<user_waypoint> user_database::load_waypoints() const
    {
        std::scoped_lock lock(pimpl->mutex);
        std::vector<user_waypoint> out;
        auto& st = pimpl->stmt_load_user_waypoints;
        st.reset();
        while(st.step())
        {
            user_waypoint w;
            w.waypoint_id = st.column_int64(0);
            w.name = st.column_text(1);
            w.lat = st.column_double(2);
            w.lon = st.column_double(3);
            out.push_back(std::move(w));
        }
        return out;
    }

    std::optional<user_waypoint> user_database::query_waypoint(std::int64_t waypoint_id) const
    {
        std::scoped_lock lock(pimpl->mutex);
        auto& st = pimpl->stmt_query_user_waypoint;
        st.reset();
        st.bind(1, waypoint_id);
        if(!st.step())
        {
            return std::nullopt;
        }
        user_waypoint w;
        w.waypoint_id = st.column_int64(0);
        w.name = st.column_text(1);
        w.lat = st.column_double(2);
        w.lon = st.column_double(3);
        // Release the implicit read transaction held by a statement
        // parked on SQLITE_ROW — see query_route for the rationale.
        st.reset();
        return w;
    }

    user_waypoint user_database::insert_waypoint(double lat, double lon)
    {
        std::scoped_lock lock(pimpl->mutex);

        // Auto-name with the lowest unused WPT<n>. The mutex serializes
        // every user_database call, so this scan-then-insert can't race.
        std::unordered_set<int> used;
        {
            auto& names = pimpl->stmt_user_waypoint_names;
            names.reset();
            while(names.step())
            {
                if(const auto n = wpt_name_number(names.column_text(0)))
                {
                    used.insert(*n);
                }
            }
        }
        int n = 1;
        while(used.count(n) != 0)
        {
            ++n;
        }
        const auto name = "WPT" + std::to_string(n);

        const auto ts = now_iso8601();
        auto& s = pimpl->stmt_insert_user_waypoint;
        s.reset();
        s.bind(1, name);
        s.bind(2, lat);
        s.bind(3, lon);
        s.bind(4, ts);
        s.bind(5, ts);
        s.step();
        return user_waypoint{pimpl->db.last_insert_rowid(), name, lat, lon};
    }

    bool user_database::update_waypoint(std::int64_t waypoint_id, const std::string& name, double lat, double lon)
    {
        std::scoped_lock lock(pimpl->mutex);
        if(name.empty())
        {
            return false;
        }
        {
            auto& taken = pimpl->stmt_user_waypoint_name_taken;
            taken.reset();
            taken.bind(1, name);
            taken.bind(2, waypoint_id);
            if(taken.step())
            {
                // Release the implicit read transaction held by a
                // statement parked on SQLITE_ROW — see query_route for
                // the rationale.
                taken.reset();
                return false;
            }
        }
        auto& s = pimpl->stmt_update_user_waypoint;
        s.reset();
        s.bind(1, name);
        s.bind(2, lat);
        s.bind(3, lon);
        s.bind(4, now_iso8601());
        s.bind(5, waypoint_id);
        s.step();
        return true;
    }

    void user_database::delete_waypoint(std::int64_t waypoint_id)
    {
        std::scoped_lock lock(pimpl->mutex);
        auto& s = pimpl->stmt_delete_user_waypoint;
        s.reset();
        s.bind(1, waypoint_id);
        s.step();
    }
}
