#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "ephemeral_database.hpp"
#include "tfr_refresher.hpp"

#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

using osect::airspace_point;
using osect::ephemeral_database;
using osect::tfr;
using osect::tfr_area;
using osect::tfr_refresher;

namespace osect
{
    // Stub for wake_main_thread — the production impl lives in
    // program.cpp, which transitively pulls in the whole app
    // graph (sdl, imgui, map_widget, ...). Tests don't link
    // program.cpp; the wake is a render-loop optimization, so a
    // no-op is fine here.
    void wake_main_thread() {}
}

namespace
{
    struct tmp_dir
    {
        std::filesystem::path path;
        explicit tmp_dir(const char* tag)
        {
            const auto base = std::filesystem::temp_directory_path() /
                ("osect_tfr_refresher_test_" + std::string(tag) + "_");
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

        std::filesystem::path db_file() const { return path / "ephemeral.db"; }
    };

    tfr make_test_tfr()
    {
        tfr t{};
        t.tfr_id = 7;
        t.notam_id = "9/9999";
        t.tfr_type = "99.7";
        t.facility = "ZOA";
        t.date_expire = "2099-01-01T00:00:00";
        t.description = "synthetic test fixture";
        tfr_area a{};
        a.area_id = 1;
        a.area_name = "Area A";
        a.upper_ft_val = 1000;
        a.upper_ft_ref = "MSL";
        a.lower_ft_val = 0;
        a.lower_ft_ref = "SFC";
        // Tiny triangle.
        a.points.push_back(airspace_point{37.0, -122.0});
        a.points.push_back(airspace_point{37.1, -122.0});
        a.points.push_back(airspace_point{37.0, -121.9});
        t.areas.push_back(std::move(a));
        return t;
    }
}

TEST_CASE("tfr_refresher: empty database leaves last_updated unset")
{
    tmp_dir dir("empty");
    tfr_refresher src(/*offline=*/true, dir.db_file());
    CHECK(!src.last_updated().has_value());

    // Verify through a separate read connection that no TFR rows
    // exist — the refresher only writes through refresh(), and
    // offline+empty means no write happened.
    ephemeral_database reader(dir.db_file());
    CHECK(reader.query_tfrs().empty());
}

TEST_CASE("tfr_refresher: existing database rows are visible to readers")
{
    tmp_dir dir("loaded");

    // Pre-populate the database through a separate connection
    // before the refresher constructs its own.
    const auto now = std::chrono::system_clock::now();
    {
        ephemeral_database db(dir.db_file());
        db.replace_tfrs({make_test_tfr()});
        db.set_source_meta("tfr", now, "");
    }

    tfr_refresher src(/*offline=*/true, dir.db_file());

    // Refresher reports the freshness it inherited from SOURCE_META.
    REQUIRE(src.last_updated().has_value());

    // Any read connection (modeling feature_builder / map_widget)
    // sees the same TFR rows.
    ephemeral_database reader(dir.db_file());
    auto rows = reader.query_tfrs();
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].notam_id == "9/9999");
    CHECK(rows[0].facility == "ZOA");
    REQUIRE(rows[0].areas.size() == 1);
    CHECK(rows[0].areas[0].upper_ft_val == 1000);
    CHECK(rows[0].areas[0].points.size() == 3);
}

TEST_CASE("tfr_refresher: refresh in offline mode preserves database state")
{
    tmp_dir dir("offline");

    {
        ephemeral_database db(dir.db_file());
        db.replace_tfrs({make_test_tfr()});
        db.set_source_meta("tfr", std::chrono::system_clock::now(), "");
    }

    tfr_refresher src(/*offline=*/true, dir.db_file());

    // Offline refresh should swallow the http error and leave the
    // database contents intact.
    src.refresh();

    ephemeral_database reader(dir.db_file());
    auto rows = reader.query_tfrs();
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].notam_id == "9/9999");
}
