#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "user_database.hpp"

#include <filesystem>
#include <sqlite/database.hpp>
#include <sqlite/statement.hpp>
#include <stdexcept>
#include <string>

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
}

TEST_CASE("user_database creates schema on fresh file and round-trips one route")
{
    tmp_dir tmp("fresh");
    {
        user_database db(tmp.db_file());
        CHECK(db.load_routes().empty());

        const auto id = db.insert_route("KCNY V8 KSGU");
        CHECK(id >= 1);

        auto rows = db.load_routes();
        REQUIRE(rows.size() == 1);
        CHECK(rows[0].route_id == id);
        CHECK(rows[0].name.empty());
        CHECK(rows[0].text == "KCNY V8 KSGU");
    }
    // Reopen the same file — schema persists, row persists.
    {
        user_database db(tmp.db_file());
        auto rows = db.load_routes();
        REQUIRE(rows.size() == 1);
        CHECK(rows[0].text == "KCNY V8 KSGU");
    }
}

TEST_CASE("insert_route returns strictly monotonic ids that survive deletion")
{
    tmp_dir tmp("ids");
    user_database db(tmp.db_file());

    const auto id1 = db.insert_route("A");
    const auto id2 = db.insert_route("B");
    const auto id3 = db.insert_route("C");
    CHECK(id2 > id1);
    CHECK(id3 > id2);

    db.delete_route(id2);

    const auto id4 = db.insert_route("D");
    CHECK(id4 > id3); // AUTOINCREMENT guarantee — id2 is NOT reused

    auto rows = db.load_routes();
    REQUIRE(rows.size() == 3);
    CHECK(rows[0].route_id == id1);
    CHECK(rows[0].text == "A");
    CHECK(rows[1].route_id == id3);
    CHECK(rows[1].text == "C");
    CHECK(rows[2].route_id == id4);
    CHECK(rows[2].text == "D");
}

TEST_CASE("update_route changes text and bumps updated_at")
{
    tmp_dir tmp("update");
    user_database db(tmp.db_file());

    const auto id = db.insert_route("OLD");
    db.update_route(id, "NEW");

    auto rows = db.load_routes();
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].text == "NEW");

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

    db.update_route(9999, "ghost");
    db.delete_route(9999);

    CHECK(db.load_routes().empty());
}

TEST_CASE("opening a database with a newer schema version throws")
{
    tmp_dir tmp("newer");
    // Create a fresh user.db, then tamper with SCHEMA_VERSIONS to
    // look like a future build wrote it.
    {
        user_database db(tmp.db_file());
        db.insert_route("preserve me");
    }
    {
        sqlite::database raw(tmp.db_file().string().c_str(), /*read_only=*/false);
        raw.exec("UPDATE SCHEMA_VERSIONS SET version = 999 WHERE group_name = 'routes'");
    }
    CHECK_THROWS_AS(user_database(tmp.db_file()), std::runtime_error);
}
