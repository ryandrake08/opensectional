#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "user_database.hpp"

#include <filesystem>
#include <initializer_list>
#include <sqlite/database.hpp>
#include <sqlite/statement.hpp>
#include <stdexcept>
#include <string>
#include <vector>

using osect::route_waypoint_row;
using osect::user_database;

namespace
{
    struct tmp_dir
    {
        std::filesystem::path path;
        explicit tmp_dir(const char* tag)
        {
            const auto base = std::filesystem::temp_directory_path() /
                              ("osect_user_db_test_" + std::string(tag) + "_");
            for(int i = 0; i < 1000; ++i)
            {
                const auto candidate = base.string() + std::to_string(i);
                if(!std::filesystem::exists(candidate))
                {
                    path = candidate;
                    std::filesystem::create_directories(path);
                    return;
                }
            }
            throw std::runtime_error("could not pick a tmp dir");
        }
        ~tmp_dir()
        {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }
        tmp_dir(const tmp_dir&) = delete;
        tmp_dir& operator=(const tmp_dir&) = delete;

        std::filesystem::path db_file() const
        {
            return path / "user.db";
        }
    };

    // A minimal valid resolved route: standalone fix waypoints, one
    // element each, with synthetic-but-distinct coordinates. The
    // user_database layer is agnostic to waypoint content — it only
    // stores and returns the rows — so this is enough to exercise it
    // without an nasr_database.
    std::vector<route_waypoint_row> make_rows(std::initializer_list<const char*> ids)
    {
        std::vector<route_waypoint_row> rows;
        int element_index = 0;
        for(const char* id : ids)
        {
            route_waypoint_row r;
            r.element_index = element_index;
            r.kind          = "fix";
            r.identifier    = id;
            r.lat           = 10.0 + element_index;
            r.lon           = -100.0 - element_index;
            r.airway_id     = std::nullopt;
            rows.push_back(r);
            ++element_index;
        }
        return rows;
    }
}

TEST_CASE("user_database creates schema on fresh file and round-trips one route")
{
    tmp_dir tmp("fresh");
    {
        user_database db(tmp.db_file());
        CHECK(db.load_routes().empty());

        const auto id = db.insert_route(make_rows({"KCNY", "KSGU"}));
        CHECK(id >= 1);

        auto routes = db.load_routes();
        REQUIRE(routes.size() == 1);
        CHECK(routes[0].route_id == id);
        CHECK(routes[0].name.empty());
        REQUIRE(routes[0].waypoints.size() == 2);
        CHECK(routes[0].waypoints[0].identifier == "KCNY");
        CHECK(routes[0].waypoints[1].identifier == "KSGU");
    }
    // Reopen the same file — schema persists, row persists.
    {
        user_database db(tmp.db_file());
        auto routes = db.load_routes();
        REQUIRE(routes.size() == 1);
        REQUIRE(routes[0].waypoints.size() == 2);
        CHECK(routes[0].waypoints[0].identifier == "KCNY");
    }
}

TEST_CASE("waypoint rows round-trip every field, including airway grouping")
{
    tmp_dir tmp("waypoints");
    user_database db(tmp.db_file());

    // O61 — V459{LIN, LOPES} — KTSP: a standalone waypoint, an
    // airway traversal of two rows sharing element_index 1, then
    // another standalone waypoint.
    std::vector<route_waypoint_row> rows;
    rows.push_back({0, "airport", "O61", 37.0, -120.0, std::nullopt});
    rows.push_back({1, "navaid", "LIN", 38.0, -121.0, std::optional<std::string>("V459")});
    rows.push_back({1, "fix", "LOPES", 38.5, -121.5, std::optional<std::string>("V459")});
    rows.push_back({2, "airport", "KTSP", 39.0, -122.0, std::nullopt});

    const auto id = db.insert_route(rows);
    const auto rec = db.query_route(id);
    REQUIRE(rec);
    REQUIRE(rec->waypoints.size() == 4);
    for(std::size_t i = 0; i < rows.size(); ++i)
    {
        CHECK(rec->waypoints[i].element_index == rows[i].element_index);
        CHECK(rec->waypoints[i].kind == rows[i].kind);
        CHECK(rec->waypoints[i].identifier == rows[i].identifier);
        CHECK(rec->waypoints[i].lat == rows[i].lat);
        CHECK(rec->waypoints[i].lon == rows[i].lon);
        CHECK(rec->waypoints[i].airway_id == rows[i].airway_id);
    }
}

TEST_CASE("query_route returns nullopt for an unknown id")
{
    tmp_dir tmp("query_miss");
    user_database db(tmp.db_file());
    db.insert_route(make_rows({"A", "B"}));
    CHECK_FALSE(db.query_route(9999));
}

TEST_CASE("insert_route returns strictly monotonic ids that survive deletion")
{
    tmp_dir tmp("ids");
    user_database db(tmp.db_file());

    const auto id1 = db.insert_route(make_rows({"A", "B"}));
    const auto id2 = db.insert_route(make_rows({"C", "D"}));
    const auto id3 = db.insert_route(make_rows({"E", "F"}));
    CHECK(id2 > id1);
    CHECK(id3 > id2);

    db.delete_route(id2);

    const auto id4 = db.insert_route(make_rows({"G", "H"}));
    CHECK(id4 > id3); // AUTOINCREMENT guarantee — id2 is NOT reused

    auto routes = db.load_routes();
    REQUIRE(routes.size() == 3);
    CHECK(routes[0].route_id == id1);
    CHECK(routes[0].waypoints.front().identifier == "A");
    CHECK(routes[1].route_id == id3);
    CHECK(routes[1].waypoints.front().identifier == "E");
    CHECK(routes[2].route_id == id4);
    CHECK(routes[2].waypoints.front().identifier == "G");
}

TEST_CASE("delete_route cascades to the route's waypoint rows")
{
    tmp_dir tmp("cascade");
    user_database db(tmp.db_file());
    const auto id = db.insert_route(make_rows({"A", "B", "C"}));
    db.delete_route(id);

    CHECK(db.load_routes().empty());

    // No orphaned ROUTE_WAYPOINT rows left behind.
    sqlite::database raw(tmp.db_file().string().c_str(), /*read_only=*/true);
    auto st = raw.prepare("SELECT COUNT(*) FROM ROUTE_WAYPOINT");
    REQUIRE(st.step());
    CHECK(st.column_int(0) == 0);
}

TEST_CASE("update_route replaces the waypoints and bumps updated_at")
{
    tmp_dir tmp("update");
    user_database db(tmp.db_file());

    const auto id = db.insert_route(make_rows({"OLD1", "OLD2"}));
    db.update_route(id, make_rows({"NEW1", "NEW2", "NEW3"}));

    auto routes = db.load_routes();
    REQUIRE(routes.size() == 1);
    REQUIRE(routes[0].waypoints.size() == 3);
    CHECK(routes[0].waypoints[0].identifier == "NEW1");
    CHECK(routes[0].waypoints[2].identifier == "NEW3");

    // created_at / updated_at aren't exposed via load_routes (the
    // runtime doesn't need them), so peek directly via sqlite.
    // ISO 8601 sorts correctly lexically, so updated_at >= created_at
    // is the right invariant after update_route.
    sqlite::database raw(tmp.db_file().string().c_str(), /*read_only=*/true);
    auto st = raw.prepare("SELECT created_at, updated_at FROM ROUTE WHERE route_id = ?");
    st.bind(1, id);
    REQUIRE(st.step());
    const auto created = st.column_text(0);
    const auto updated = st.column_text(1);
    CHECK(updated >= created);
}

TEST_CASE("update_route and delete_route are no-ops on unknown ids")
{
    tmp_dir tmp("noop");
    user_database db(tmp.db_file());

    db.update_route(9999, make_rows({"ghost1", "ghost2"}));
    db.delete_route(9999);

    CHECK(db.load_routes().empty());
}

TEST_CASE("an older schema version is dropped and recreated at the current version")
{
    tmp_dir tmp("older");
    // Hand-build a v1-shaped routes group: the SCHEMA_VERSIONS row
    // plus a ROUTE table with the old text column and a row in it.
    {
        sqlite::database raw(tmp.db_file().string().c_str(), /*read_only=*/false);
        raw.exec("CREATE TABLE SCHEMA_VERSIONS (group_name TEXT PRIMARY KEY, version INTEGER NOT NULL)");
        raw.exec("INSERT INTO SCHEMA_VERSIONS (group_name, version) VALUES ('routes', 1)");
        raw.exec("CREATE TABLE ROUTE (route_id INTEGER PRIMARY KEY, text TEXT NOT NULL)");
        raw.exec("INSERT INTO ROUTE (text) VALUES ('KCNY V8 KSGU')");
    }
    // Opening it brings the group to the current version: no users in
    // the field, so the v1 tables are dropped and recreated empty.
    {
        user_database db(tmp.db_file());
        CHECK(db.load_routes().empty());
        const auto id = db.insert_route(make_rows({"A", "B"}));
        CHECK(id >= 1);
    }
    sqlite::database raw(tmp.db_file().string().c_str(), /*read_only=*/true);
    auto ver = raw.prepare("SELECT version FROM SCHEMA_VERSIONS WHERE group_name = 'routes'");
    REQUIRE(ver.step());
    CHECK(ver.column_int(0) == 2);
    // The v2 ROUTE_WAYPOINT table exists.
    auto tbl = raw.prepare("SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='ROUTE_WAYPOINT'");
    REQUIRE(tbl.step());
    CHECK(tbl.column_int(0) == 1);
}

TEST_CASE("opening a database with a newer schema version throws")
{
    tmp_dir tmp("newer");
    // Create a fresh user.db, then tamper with SCHEMA_VERSIONS to
    // look like a future build wrote it.
    {
        user_database db(tmp.db_file());
        db.insert_route(make_rows({"AAA", "BBB"}));
    }
    {
        sqlite::database raw(tmp.db_file().string().c_str(), /*read_only=*/false);
        raw.exec("UPDATE SCHEMA_VERSIONS SET version = 999 WHERE group_name = 'routes'");
    }
    CHECK_THROWS_AS(user_database(tmp.db_file()), std::runtime_error);
}
