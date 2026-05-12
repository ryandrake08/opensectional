#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "ephemeral_database.hpp"

#include <chrono>
#include <filesystem>
#include <sqlite/database.hpp>
#include <sqlite/statement.hpp>
#include <stdexcept>
#include <string>
#include <vector>

using osect::airspace_point;
using osect::ephemeral_database;
using osect::tfr;
using osect::tfr_area;

namespace
{
    struct tmp_dir
    {
        std::filesystem::path path;
        explicit tmp_dir(const char* tag)
        {
            const auto base = std::filesystem::temp_directory_path() /
                ("osect_ephemeral_db_test_" + std::string(tag) + "_");
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

    // area_id is per-TFR sequential (1, 2, 3, ...) — both fixtures
    // start their areas at 1 to exercise the composite (tfr_id,
    // area_id) PK. A globally-unique area_id schema would fail on the
    // second TFR's insert with a UNIQUE constraint error.
    tfr make_tfr_a()
    {
        tfr t{};
        t.tfr_id              = 1;
        t.notam_id            = "1/0001";
        t.tfr_type            = "SECURITY";
        t.facility            = "ZOA";
        t.date_effective      = "2026-05-01T00:00:00";
        t.date_expire         = "2099-01-01T00:00:00";
        t.description         = "fixture A";
        t.date_issued         = "2026-04-30T12:00:00";
        t.city                = "San Francisco";
        t.state               = "CALIFORNIA";
        t.coord_facility      = "ZOA";
        t.coord_facility_name = "Oakland Center";
        t.coord_facility_type = "ARTCC";
        t.coord_phone         = "555-1234";
        t.coord_freq          = "125.35";
        t.poc_name            = "John Doe";
        t.poc_org             = "USSS";
        t.poc_phone           = "555-5678";
        t.poc_freq            = "123.45";
        t.time_zone           = "PDT";
        t.expire_time_zone    = "PST";

        tfr_area a1{};
        a1.area_id        = 1;
        a1.area_name      = "Area 1";
        a1.upper_ft_val   = 1000;
        a1.upper_ft_ref   = "MSL";
        a1.lower_ft_val   = 0;
        a1.lower_ft_ref   = "SFC";
        a1.date_effective   = "2026-05-01T00:00:00";
        a1.date_expire      = "2099-01-01T00:00:00";
        a1.start_time       = "1300";
        a1.end_time         = "2200";
        a1.is_time_separate = "TRUE";
        a1.day_code         = "MTWTF";
        a1.instructions     = "Contact ZOA on 125.35 prior to entering.\nLine two.";
        a1.points.push_back(airspace_point{37.0, -122.0});
        a1.points.push_back(airspace_point{37.1, -122.0});
        a1.points.push_back(airspace_point{37.0, -121.9});
        t.areas.push_back(std::move(a1));

        tfr_area a2{};
        a2.area_id        = 2;
        a2.area_name      = "Area 2";
        a2.upper_ft_val   = 5000;
        a2.upper_ft_ref   = "MSL";
        a2.lower_ft_val   = 1000;
        a2.lower_ft_ref   = "MSL";
        a2.points.push_back(airspace_point{38.0, -123.0});
        a2.points.push_back(airspace_point{38.2, -123.0});
        a2.points.push_back(airspace_point{38.2, -122.8});
        a2.points.push_back(airspace_point{38.0, -122.8});
        t.areas.push_back(std::move(a2));

        return t;
    }

    tfr make_tfr_b()
    {
        tfr t{};
        t.tfr_id    = 2;
        t.notam_id  = "1/0002";
        t.tfr_type  = "VIP";
        t.facility  = "ZLA";
        t.description = "fixture B";

        tfr_area a{};
        a.area_id      = 1;
        a.area_name    = "B-only area";
        a.upper_ft_val = 17999;
        a.upper_ft_ref = "MSL";
        a.points.push_back(airspace_point{34.0, -118.0});
        a.points.push_back(airspace_point{34.1, -118.0});
        a.points.push_back(airspace_point{34.0, -117.9});
        t.areas.push_back(std::move(a));

        return t;
    }
}

TEST_CASE("ephemeral_database: fresh open creates empty schema")
{
    tmp_dir d("fresh");
    ephemeral_database db(d.db_file());
    CHECK(db.query_tfrs().empty());
    CHECK(!db.last_refreshed("tfr").has_value());
    CHECK(db.etag("tfr").empty());
}

TEST_CASE("ephemeral_database: replace_tfrs + query_tfrs round-trip")
{
    tmp_dir d("roundtrip");
    ephemeral_database db(d.db_file());

    const std::vector<tfr> in = {make_tfr_a(), make_tfr_b()};
    db.replace_tfrs(in);

    const auto out = db.query_tfrs();
    REQUIRE(out.size() == in.size());

    for(std::size_t i = 0; i < in.size(); ++i)
    {
        CHECK(out[i].tfr_id              == in[i].tfr_id);
        CHECK(out[i].notam_id            == in[i].notam_id);
        CHECK(out[i].tfr_type            == in[i].tfr_type);
        CHECK(out[i].facility            == in[i].facility);
        CHECK(out[i].date_effective      == in[i].date_effective);
        CHECK(out[i].date_expire         == in[i].date_expire);
        CHECK(out[i].description         == in[i].description);
        CHECK(out[i].date_issued         == in[i].date_issued);
        CHECK(out[i].city                == in[i].city);
        CHECK(out[i].state               == in[i].state);
        CHECK(out[i].coord_facility      == in[i].coord_facility);
        CHECK(out[i].coord_facility_name == in[i].coord_facility_name);
        CHECK(out[i].coord_facility_type == in[i].coord_facility_type);
        CHECK(out[i].coord_phone         == in[i].coord_phone);
        CHECK(out[i].coord_freq          == in[i].coord_freq);
        CHECK(out[i].poc_name            == in[i].poc_name);
        CHECK(out[i].poc_org             == in[i].poc_org);
        CHECK(out[i].poc_phone           == in[i].poc_phone);
        CHECK(out[i].poc_freq            == in[i].poc_freq);
        CHECK(out[i].time_zone           == in[i].time_zone);
        CHECK(out[i].expire_time_zone    == in[i].expire_time_zone);

        REQUIRE(out[i].areas.size() == in[i].areas.size());
        for(std::size_t j = 0; j < in[i].areas.size(); ++j)
        {
            const auto& a_in  = in[i].areas[j];
            const auto& a_out = out[i].areas[j];
            CHECK(a_out.area_id        == a_in.area_id);
            CHECK(a_out.area_name      == a_in.area_name);
            CHECK(a_out.upper_ft_val   == a_in.upper_ft_val);
            CHECK(a_out.upper_ft_ref   == a_in.upper_ft_ref);
            CHECK(a_out.lower_ft_val   == a_in.lower_ft_val);
            CHECK(a_out.lower_ft_ref   == a_in.lower_ft_ref);
            CHECK(a_out.date_effective   == a_in.date_effective);
            CHECK(a_out.date_expire      == a_in.date_expire);
            CHECK(a_out.start_time       == a_in.start_time);
            CHECK(a_out.end_time         == a_in.end_time);
            CHECK(a_out.is_time_separate == a_in.is_time_separate);
            CHECK(a_out.day_code         == a_in.day_code);
            CHECK(a_out.instructions     == a_in.instructions);

            REQUIRE(a_out.points.size() == a_in.points.size());
            for(std::size_t k = 0; k < a_in.points.size(); ++k)
            {
                CHECK(a_out.points[k].lat == a_in.points[k].lat);
                CHECK(a_out.points[k].lon == a_in.points[k].lon);
            }
        }
    }
}

TEST_CASE("ephemeral_database: replace_tfrs is atomic — no stale rows leak")
{
    tmp_dir d("atomic");
    ephemeral_database db(d.db_file());

    db.replace_tfrs({make_tfr_a(), make_tfr_b()});
    REQUIRE(db.query_tfrs().size() == 2);

    // Replace with a strict subset — the prior fixture's areas / points
    // must not survive.
    db.replace_tfrs({make_tfr_b()});
    const auto out = db.query_tfrs();
    REQUIRE(out.size() == 1);
    CHECK(out[0].notam_id == "1/0002");
    REQUIRE(out[0].areas.size() == 1);
    CHECK(out[0].areas[0].area_id == 1);
    CHECK(out[0].areas[0].points.size() == 3);
}

TEST_CASE("ephemeral_database: source_meta round-trip")
{
    tmp_dir d("meta");
    ephemeral_database db(d.db_file());

    // System clock resolution might exceed seconds; format_iso8601
    // truncates to 1-second granularity, so build a time_point that
    // doesn't have sub-second components for an exact comparison.
    using namespace std::chrono;
    const auto stamp = time_point_cast<seconds>(system_clock::now());

    db.set_source_meta("tfr", stamp, "etag-abc");

    auto got = db.last_refreshed("tfr");
    REQUIRE(got.has_value());
    CHECK(*got == stamp);
    CHECK(db.etag("tfr") == "etag-abc");

    // Update in place.
    const auto stamp2 = stamp + minutes(5);
    db.set_source_meta("tfr", stamp2, "etag-xyz");
    got = db.last_refreshed("tfr");
    REQUIRE(got.has_value());
    CHECK(*got == stamp2);
    CHECK(db.etag("tfr") == "etag-xyz");

    // Unknown source returns nullopt / empty.
    CHECK(!db.last_refreshed("notam").has_value());
    CHECK(db.etag("notam").empty());
}

TEST_CASE("ephemeral_database: schema-version mismatch rebuilds tfr group, leaves other rows intact")
{
    tmp_dir d("mismatch");

    {
        ephemeral_database db(d.db_file());
        db.replace_tfrs({make_tfr_a()});
        db.set_source_meta("tfr",
                           std::chrono::system_clock::from_time_t(1700000000),
                           "old-etag");
        REQUIRE(db.query_tfrs().size() == 1);
    }

    // Reach into the file directly: bump the on-disk TFR version so the
    // next open is forced to rebuild. Drop a synthetic non-tfr row into
    // SOURCE_META that must survive the rebuild — proving the per-group
    // policy doesn't touch unrelated sources.
    {
        sqlite::database raw(d.db_file().string().c_str(), /*read_only=*/false);
        raw.exec("UPDATE SCHEMA_VERSIONS SET version = 'stale' WHERE group_name = 'tfr'");
        raw.exec("INSERT INTO SOURCE_META (name, last_refreshed, etag) "
                 "VALUES ('canary', '2026-01-02T03:04:05Z', 'canary-etag')");
    }

    {
        ephemeral_database db(d.db_file());
        CHECK(db.query_tfrs().empty());
        // The rebuild wipes the prior SOURCE_META row for "tfr" as part
        // of the group reset — freshness is meaningless after a wipe.
        CHECK(!db.last_refreshed("tfr").has_value());
        CHECK(db.etag("tfr").empty());

        // The unrelated row survives.
        CHECK(db.etag("canary") == "canary-etag");
        CHECK(db.last_refreshed("canary").has_value());
    }
}
