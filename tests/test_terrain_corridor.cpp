#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "elevation_address.hpp"
#include "elevation_source.hpp"
#include "elevation_test_tree.hpp"
#include "nasr_database.hpp"
#include "terrain_corridor.hpp"

#include <algorithm>
#include <string>

namespace
{
    const osect::nasr_database& test_db()
    {
        static const osect::nasr_database db("osect.db");
        return db;
    }

    constexpr double HUNDRED_METRES_FT = 328.0839895013123;
    constexpr double LINE_LAT = 0.005;
    constexpr int PEAK_ZOOM = 15;

    // A 12 NM east-west centreline at LINE_LAT with a 1 NM half-width,
    // in two intervals.
    std::vector<osect::corridor_station> east_west_stations()
    {
        return {{0.0, {LINE_LAT, 0.0}, 1.0}, {6.0, {LINE_LAT, 0.1}, 1.0}, {12.0, {LINE_LAT, 0.2}, 1.0}};
    }

    std::string peak_manifest(const char* name)
    {
        return osect::test::manifest(name, 1, 0, PEAK_ZOOM, PEAK_ZOOM, 1.0);
    }

    // Writes the tree's only tile, 100 m high, containing the point
    // `north_nm` north of the centreline at longitude 0.05, within the
    // first interval. Zoom 15 tiles are about 0.66 NM across.
    void write_peak(const osect::test::temporary_tree& tree, double north_nm)
    {
        const auto peak = osect::address_elevation(LINE_LAT + north_nm / 60.0, 0.05, PEAK_ZOOM, 1, 0).key;
        osect::test::write_elevation_tile(tree.path() / std::to_string(PEAK_ZOOM) / std::to_string(peak.x) /
                                              (std::to_string(peak.y) + ".png"),
                                          true);
    }

    // Starting at (lat, lon), moves to the tallest obstacle within half a
    // degree until none nearby is taller.
    osect::obstacle locally_tallest_obstacle(double lat, double lon)
    {
        const auto taller = [](const auto& a, const auto& b) { return a.amsl_ht < b.amsl_ht; };
        const auto nearby = [](double lat, double lon)
        { return test_db().query_obstacles({lon - 0.5, lat - 0.5, lon + 0.5, lat + 0.5}); };
        auto candidates = nearby(lat, lon);
        REQUIRE_FALSE(candidates.empty());
        auto tallest = *std::max_element(candidates.begin(), candidates.end(), taller);
        for(;;)
        {
            candidates = nearby(tallest.lat, tallest.lon);
            const auto next = *std::max_element(candidates.begin(), candidates.end(), taller);
            if(next.amsl_ht <= tallest.amsl_ht)
            {
                return tallest;
            }
            tallest = next;
        }
    }

    // Two stations 6 NM apart either side of `lon` at `lat`, 1 NM
    // half-width.
    std::vector<osect::corridor_station> stations_across(double lat, double lon)
    {
        return {{0.0, {lat, lon - 0.05}, 1.0}, {6.0, {lat, lon + 0.05}, 1.0}};
    }
}

TEST_CASE("corridor half-width is half the width away from the route ends")
{
    CHECK(osect::corridor_half_width_nm(8.0, 30.0, 50.0) == 4.0);
}

TEST_CASE("corridor half-width narrows linearly to zero at either route end")
{
    CHECK(osect::corridor_half_width_nm(8.0, 0.0, 50.0) == 0.0);
    CHECK(osect::corridor_half_width_nm(8.0, 5.0, 50.0) == 2.0);
    CHECK(osect::corridor_half_width_nm(8.0, 50.0, 5.0) == 2.0);
    CHECK(osect::corridor_half_width_nm(8.0, 50.0, 0.0) == 0.0);
}

TEST_CASE("corridor intervals report terrain inside the corridor")
{
    const osect::test::temporary_tree tree("corridor-inside", peak_manifest("inside"));
    write_peak(tree, 0.5);
    const osect::elevation_source terrain(tree.path());
    const auto intervals = osect::corridor_intervals(east_west_stations(), terrain, test_db(), false);

    REQUIRE(intervals.size() == 2);
    REQUIRE(intervals[0].terrain_ft);
    CHECK(*intervals[0].terrain_ft == doctest::Approx(HUNDRED_METRES_FT)); // metres to feet
    CHECK_FALSE(intervals[1].terrain_ft);
}

TEST_CASE("corridor intervals ignore terrain beyond the half-width")
{
    const osect::test::temporary_tree tree("corridor-outside", peak_manifest("outside"));
    write_peak(tree, 3.0);
    const osect::elevation_source terrain(tree.path());
    const auto intervals = osect::corridor_intervals(east_west_stations(), terrain, test_db(), false);

    REQUIRE(intervals.size() == 2);
    CHECK_FALSE(intervals[0].terrain_ft);
    CHECK_FALSE(intervals[1].terrain_ft);
}

TEST_CASE("corridor intervals report no terrain where the tree has no data")
{
    const osect::elevation_source terrain("missing-terrain-tree");
    const auto intervals = osect::corridor_intervals(east_west_stations(), terrain, test_db(), false);

    REQUIRE(intervals.size() == 2);
    CHECK_FALSE(intervals[0].terrain_ft);
    CHECK_FALSE(intervals[1].terrain_ft);
}

TEST_CASE("corridor intervals mark corridors inside the tree's coverage")
{
    // Coverage ends at longitude 0.15: the first interval's window spans
    // about -0.017 to 0.117, the second about 0.083 to 0.217.
    const osect::test::temporary_tree tree(
        "corridor-coverage",
        osect::test::manifest("coverage", 1, 0, PEAK_ZOOM, PEAK_ZOOM, 1.0, "EGM2008", {}, false, "[-1, -1, 0.15, 1]"));
    const osect::elevation_source terrain(tree.path());
    const auto intervals = osect::corridor_intervals(east_west_stations(), terrain, test_db(), false);

    REQUIRE(intervals.size() == 2);
    CHECK(intervals[0].covered);
    CHECK_FALSE(intervals[1].covered);
}

TEST_CASE("corridor intervals report the tallest obstacle in the corridor")
{
    const auto tallest = locally_tallest_obstacle(37.5, -122.0);
    const osect::elevation_source terrain("missing-terrain-tree");
    const auto intervals =
        osect::corridor_intervals(stations_across(tallest.lat, tallest.lon), terrain, test_db(), true);

    REQUIRE(intervals.size() == 1);
    CHECK(intervals[0].obstacle_ft == static_cast<double>(tallest.amsl_ht));
}

TEST_CASE("corridor intervals omit obstacles when asked to")
{
    const auto tallest = locally_tallest_obstacle(37.5, -122.0);
    const osect::elevation_source terrain("missing-terrain-tree");
    const auto intervals =
        osect::corridor_intervals(stations_across(tallest.lat, tallest.lon), terrain, test_db(), false);

    REQUIRE(intervals.size() == 1);
    CHECK_FALSE(intervals[0].obstacle_ft);
}

TEST_CASE("corridor intervals find obstacles past the antimeridian")
{
    // Near Adak, with station longitudes continued east past 180 degrees,
    // as on a route from Shemya.
    const auto tallest = locally_tallest_obstacle(51.88, -176.65);
    const osect::elevation_source terrain("missing-terrain-tree");
    const auto intervals =
        osect::corridor_intervals(stations_across(tallest.lat, tallest.lon + 360.0), terrain, test_db(), true);

    REQUIRE(intervals.size() == 1);
    CHECK(intervals[0].obstacle_ft == static_cast<double>(tallest.amsl_ht));
}
