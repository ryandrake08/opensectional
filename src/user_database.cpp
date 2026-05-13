#include "user_database.hpp"
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <locale>
#include <mutex>
#include <sdl/log.hpp>
#include <sqlite/database.hpp>
#include <sqlite/statement.hpp>
#include <sstream>
#include <stdexcept>
#include <string>

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

        // Routes group, v1.
        //
        // route_id is AUTOINCREMENT so rowids are strictly
        // monotonic and never reused — future cross-references
        // (e.g. a planned route persisting which user_waypoint ids
        // it traverses) can rely on a row's identity remaining
        // stable even after surrounding rows are deleted.
        constexpr int ROUTES_GROUP_VERSION = 1;
        constexpr const char* ROUTES_GROUP_NAME = "routes";
        constexpr const char* ROUTES_GROUP_CREATE_V1_SQL = R"(
            CREATE TABLE ROUTE (
                route_id    INTEGER PRIMARY KEY AUTOINCREMENT,
                name        TEXT NOT NULL DEFAULT '',
                text        TEXT NOT NULL,
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

        // Bring the routes group to ROUTES_GROUP_VERSION. Four cases:
        //   missing       → CREATE v1, stamp version.
        //   == current    → no-op.
        //   < current     → walk forward through migrations (none
        //                   defined yet for v1).
        //   > current     → throw. Refuse to operate on a database
        //                   newer than this build understands.
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

            if(!present)
            {
                db.exec("BEGIN");
                try
                {
                    db.exec(ROUTES_GROUP_CREATE_V1_SQL);
                    auto ins = db.prepare("INSERT INTO SCHEMA_VERSIONS (group_name, version) VALUES (?, ?)");
                    ins.bind(1, ROUTES_GROUP_NAME);
                    ins.bind(2, ROUTES_GROUP_VERSION);
                    ins.step();
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
                return;
            }

            if(on_disk == ROUTES_GROUP_VERSION)
            {
                return;
            }

            if(on_disk > ROUTES_GROUP_VERSION)
            {
                throw std::runtime_error(
                    std::string("user.db: '") + ROUTES_GROUP_NAME + "' group is at version " +
                    std::to_string(on_disk) + " but this build only understands version " +
                    std::to_string(ROUTES_GROUP_VERSION) +
                    ". Refusing to open — upgrade the application or restore a backup.");
            }

            // on_disk < current: walk forward. No migrations exist
            // for v1; this branch is unreachable today.
            throw std::runtime_error(
                std::string("user.db: '") + ROUTES_GROUP_NAME + "' group is at version " +
                std::to_string(on_disk) + " but no forward migration is defined to version " +
                std::to_string(ROUTES_GROUP_VERSION) + ".");
        }

        sqlite::database open_and_init_schema(const std::filesystem::path& p)
        {
            sqlite::database db(p.string().c_str(), /*read_only=*/false);
            // WAL lets readers proceed without blocking on a writer,
            // matching ephemeral_database — needed once consumers
            // open their own read-only connections.
            db.exec("PRAGMA journal_mode = WAL");
            db.exec("PRAGMA foreign_keys = ON");
            db.exec(BOOTSTRAP_SQL);
            ensure_routes_group(db);
            return db;
        }
    }

    struct user_database::impl
    {
        sqlite::database db;
        mutable std::mutex mutex;

        sqlite::statement stmt_load_routes;
        sqlite::statement stmt_insert_route;
        sqlite::statement stmt_update_route;
        sqlite::statement stmt_delete_route;

        explicit impl(const std::filesystem::path& p)
            : db(open_and_init_schema(p)),
              stmt_load_routes(db.prepare(R"(
                SELECT route_id, name, text FROM ROUTE ORDER BY route_id
            )")),
              stmt_insert_route(db.prepare(R"(
                INSERT INTO ROUTE (text, created_at, updated_at) VALUES (?, ?, ?)
            )")),
              stmt_update_route(db.prepare(R"(
                UPDATE ROUTE SET text = ?, updated_at = ? WHERE route_id = ?
            )")),
              stmt_delete_route(db.prepare(R"(
                DELETE FROM ROUTE WHERE route_id = ?
            )"))
        {
        }
    };

    std::filesystem::path user_database::default_path()
    {
        return app_user_data_dir() / "user.db";
    }

    user_database::user_database(const std::filesystem::path& db_path)
        : pimpl(std::make_unique<impl>(db_path))
    {
    }

    user_database::~user_database() = default;

    std::vector<route_record> user_database::load_routes() const
    {
        std::lock_guard<std::mutex> lock(pimpl->mutex);
        std::vector<route_record> out;
        auto& st = pimpl->stmt_load_routes;
        st.reset();
        while(st.step())
        {
            route_record r;
            r.route_id = st.column_int64(0);
            r.name     = st.column_text(1);
            r.text     = st.column_text(2);
            out.push_back(std::move(r));
        }
        return out;
    }

    std::int64_t user_database::insert_route(const std::string& text)
    {
        std::lock_guard<std::mutex> lock(pimpl->mutex);
        const auto ts = now_iso8601();
        auto& s = pimpl->stmt_insert_route;
        s.reset();
        s.bind(1, text);
        s.bind(2, ts);
        s.bind(3, ts);
        s.step();
        return pimpl->db.last_insert_rowid();
    }

    void user_database::update_route(std::int64_t route_id, const std::string& text)
    {
        std::lock_guard<std::mutex> lock(pimpl->mutex);
        auto& s = pimpl->stmt_update_route;
        s.reset();
        s.bind(1, text);
        s.bind(2, now_iso8601());
        s.bind(3, route_id);
        s.step();
    }

    void user_database::delete_route(std::int64_t route_id)
    {
        std::lock_guard<std::mutex> lock(pimpl->mutex);
        auto& s = pimpl->stmt_delete_route;
        s.reset();
        s.bind(1, route_id);
        s.step();
    }

    void user_database::delete_all_routes()
    {
        std::lock_guard<std::mutex> lock(pimpl->mutex);
        pimpl->db.exec("DELETE FROM ROUTE");
    }
}
