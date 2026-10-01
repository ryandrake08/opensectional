#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "elevation_address.hpp"
#include "elevation_source.hpp"
#include "elevation_test_tree.hpp"
#include "flight_route.hpp"
#include "geo_math.hpp"
#include "nasr_database.hpp"
#include "route_planner.hpp"
#include "terrain_corridor.hpp"
#include "tmp_user_db.hpp"

#include <algorithm>
#include <cmath>
#include <string>

using namespace osect;
using rpta = route_planner_test_access;

namespace
{
    constexpr double FEET_PER_METRE = 3.280839895013123;

    const nasr_database& test_db()
    {
        static const nasr_database db("osect.db");
        return db;
    }

    std::string tree_manifest(const char* name, int zoom, const std::string& bbox = "[-180, -85, 180, 85]")
    {
        return test::manifest(name, 1, 0, zoom, zoom, 1.0, "EGM2008", {}, false, bbox);
    }

    // Writes the 1x1-pixel tile at `zoom` containing (lat, lon).
    void write_tile_at(const test::temporary_tree& tree, int zoom, double lat, double lon, double elevation_ft)
    {
        const auto key = address_elevation(lat, lon, zoom, 1, 0).key;
        test::write_elevation_tile_m(tree.path() / std::to_string(zoom) / std::to_string(key.x) /
                                         (std::to_string(key.y) + ".png"),
                                     elevation_ft / FEET_PER_METRE);
    }

    // Options for terrain avoidance at `cruise_ft` with the default
    // 8 NM corridor and 1,000 ft clearance.
    route_planner::options avoiding(std::optional<double> cruise_ft)
    {
        route_planner::options opts;
        opts.avoid_terrain = true;
        opts.cruise_altitude_ft = cruise_ft;
        return opts;
    }

    // The expanded text, or empty when no route was found.
    std::string expand_or_empty(const route_planner& planner, const std::string& text,
                                const route_planner::options& opts)
    {
        try
        {
            return planner.expand_sigils(text, opts).text;
        }
        catch(const route_parse_error&)
        {
            return {};
        }
    }

    // True when any 2 NM piece of the route's full-width 8 NM corridor
    // has terrain above `limit_ft`. Ignores the terminal tapers.
    bool route_terrain_above(const std::string& text, const elevation_source& terrain, double limit_ft)
    {
        const flight_route route(text, test_db(), {});
        for(std::size_t leg = 0; leg + 1 < route.waypoints.size(); ++leg)
        {
            const auto& from = route.waypoints[leg];
            const auto& to = route.waypoints[leg + 1];
            const double length_nm = haversine_distance_nm(from.lat, from.lon, to.lat, to.lon);
            const int pieces = std::max(1, static_cast<int>(std::ceil(length_nm / 2.0)));
            std::vector<corridor_station> stations;
            for(int i = 0; i <= pieces; ++i)
            {
                const double fraction = static_cast<double>(i) / pieces;
                stations.push_back(
                    {length_nm * fraction, geodesic_point(from.lat, from.lon, to.lat, to.lon, fraction), 4.0});
            }
            for(const auto& interval : corridor_intervals(stations, terrain, test_db(), false))
            {
                if(interval.terrain_ft && *interval.terrain_ft > limit_ft)
                {
                    return true;
                }
            }
        }
        return false;
    }

    // ---- A ridge across the Central Valley ----
    //
    // 5,000 m (16,404 ft) zoom-10 tiles in a row at 37.0 N from 120.8 W
    // to 119.8 W, across the KSMF-KBFL direct line. No other tiles, so
    // terrain elsewhere is absent. Cruise 14,000 ft clears every
    // obstacle in the valley and foothills by the required clearance.

    constexpr int RIDGE_ZOOM = 10;
    constexpr double RIDGE_LAT = 37.0;
    constexpr double RIDGE_FT = 5000.0 * FEET_PER_METRE;
    constexpr double RIDGE_CRUISE_FT = 14000.0;

    const elevation_source& ridge_terrain()
    {
        static const test::temporary_tree tree("planner-ridge", tree_manifest("ridge", RIDGE_ZOOM));
        static const elevation_source terrain = [&]
        {
            const auto west = address_elevation(RIDGE_LAT, -120.8, RIDGE_ZOOM, 1, 0).key;
            const auto east = address_elevation(RIDGE_LAT, -119.8, RIDGE_ZOOM, 1, 0).key;
            for(int x = west.x; x <= east.x; ++x)
            {
                test::write_elevation_tile_m(tree.path() / std::to_string(RIDGE_ZOOM) / std::to_string(x) /
                                                 (std::to_string(west.y) + ".png"),
                                             5000.0);
            }
            return elevation_source(tree.path());
        }();
        return terrain;
    }

    const route_planner& ridge_planner()
    {
        static test::tmp_user_db user_db("planner_ridge");
        static const route_planner planner("osect.db", user_db.db_file, ridge_terrain());
        return planner;
    }

    // ---- Points over the open Pacific at 36 N ----
    //
    // No graph nodes or obstacles lie near them, so a direct leg that
    // fails the terrain test leaves no route. B is 5' (4.05 NM) east of
    // A, C 30' (24.3 NM) east of A.

    const std::string A = "360000N1240000W";
    const std::string B = "360000N1235500W";
    const std::string C = "360000N1233000W";
    constexpr double OCEAN_LAT = 36.0;
    constexpr double A_LON = -124.0;
    constexpr int OCEAN_ZOOM = 13;
    constexpr double OCEAN_CRUISE_FT = 5000.0;

    // Longitude `nm` east of A along 36 N.
    double east_of_a(double nm)
    {
        return A_LON + nm_to_deg_lon(nm, OCEAN_LAT);
    }
}

TEST_CASE("terrain test tiles decode to the elevation written")
{
    const test::temporary_tree tree("planner-decode", tree_manifest("decode", OCEAN_ZOOM));
    write_tile_at(tree, OCEAN_ZOOM, OCEAN_LAT, east_of_a(3.0), 4500.0);
    const elevation_source terrain(tree.path());
    const auto elevation_ft = terrain.elevation_ft(OCEAN_LAT, east_of_a(3.0), OCEAN_ZOOM);
    REQUIRE(elevation_ft);
    CHECK(*elevation_ft == doctest::Approx(4500.0).epsilon(1e-5)); // 1/256 m quantization
}

TEST_CASE("a plan without terrain avoidance crosses the ridge")
{
    route_planner::options opts;
    opts.cruise_altitude_ft = RIDGE_CRUISE_FT;
    const auto expanded = ridge_planner().expand_sigils("KSMF ? KBFL", opts).text;
    CHECK(route_terrain_above(expanded, ridge_terrain(), RIDGE_CRUISE_FT - 1000.0));
}

TEST_CASE("terrain avoidance routes around the ridge")
{
    const auto expanded = ridge_planner().expand_sigils("KSMF ? KBFL", avoiding(RIDGE_CRUISE_FT)).text;
    CHECK_FALSE(route_terrain_above(expanded, ridge_terrain(), RIDGE_CRUISE_FT - 1000.0));
}

TEST_CASE("terrain avoidance crosses a ridge that cruise clears by the required clearance")
{
    route_planner::options without;
    without.cruise_altitude_ft = RIDGE_FT + 1000.0;
    CHECK(ridge_planner().expand_sigils("KSMF ? KBFL", avoiding(RIDGE_FT + 1000.0)).text ==
          ridge_planner().expand_sigils("KSMF ? KBFL", without).text);
}

TEST_CASE("terrain avoidance rejects a direct leg across the ridge")
{
    // 48 NM north-south across the ridge, well within the 80 NM max leg.
    const rpta::endpoint north{rpta::synthetic, 37.4, -120.3};
    const rpta::endpoint south{rpta::synthetic, 36.6, -120.3};

    route_planner::options without;
    without.cruise_altitude_ft = RIDGE_CRUISE_FT;
    const auto direct = rpta::plan_segment(ridge_planner(), north, south, without);
    REQUIRE(direct);
    CHECK(direct->empty());

    const auto avoided = rpta::plan_segment(ridge_planner(), north, south, avoiding(RIDGE_CRUISE_FT));
    REQUIRE(avoided);
    CHECK_FALSE(avoided->empty());
}

TEST_CASE("terrain avoidance applies to the final leg into the destination")
{
    // A destination 20 NM south of the ridge: nodes north of the ridge
    // within 80 NM of it must not be the last intermediate.
    const auto expanded = ridge_planner().expand_sigils("KSMF ? 362000N1201800W", avoiding(RIDGE_CRUISE_FT)).text;
    CHECK_FALSE(route_terrain_above(expanded, ridge_terrain(), RIDGE_CRUISE_FT - 1000.0));
}

TEST_CASE("terrain avoidance excludes a leg past an obstacle within the required clearance")
{
    // Climb to the locally tallest obstacle near the Bay Area.
    const auto taller = [](const auto& a, const auto& b) { return a.amsl_ht < b.amsl_ht; };
    const auto nearby = [](double lat, double lon)
    { return test_db().query_obstacles({lon - 0.5, lat - 0.5, lon + 0.5, lat + 0.5}); };
    auto candidates = nearby(37.5, -122.0);
    REQUIRE_FALSE(candidates.empty());
    auto tallest = *std::max_element(candidates.begin(), candidates.end(), taller);
    for(;;)
    {
        candidates = nearby(tallest.lat, tallest.lon);
        const auto next = *std::max_element(candidates.begin(), candidates.end(), taller);
        if(next.amsl_ht <= tallest.amsl_ht)
        {
            break;
        }
        tallest = next;
    }

    // A tree with no tiles: terrain is available but absent.
    const test::temporary_tree tree("planner-obstacle", tree_manifest("obstacle", OCEAN_ZOOM));
    const elevation_source terrain(tree.path());
    test::tmp_user_db user_db("planner_obstacle");
    const route_planner planner("osect.db", user_db.db_file, terrain);

    // 40 NM north-south through the obstacle: it is 20 NM from both
    // ends, outside the terminal tapers.
    const rpta::endpoint north{rpta::synthetic, tallest.lat + 20.0 / 60.0, tallest.lon};
    const rpta::endpoint south{rpta::synthetic, tallest.lat - 20.0 / 60.0, tallest.lon};

    const auto within = rpta::plan_segment(planner, north, south, avoiding(tallest.amsl_ht + 500.0));
    CHECK_FALSE((within && within->empty()));

    const auto clear = rpta::plan_segment(planner, north, south, avoiding(tallest.amsl_ht + 1000.0));
    REQUIRE(clear);
    CHECK(clear->empty());
}

TEST_CASE("terrain within the required clearance is allowed inside the departure taper")
{
    const test::temporary_tree tree("planner-departure-taper", tree_manifest("departure", OCEAN_ZOOM));
    write_tile_at(tree, OCEAN_ZOOM, OCEAN_LAT, east_of_a(3.0), OCEAN_CRUISE_FT - 500.0);
    const elevation_source terrain(tree.path());
    test::tmp_user_db user_db("planner_departure_taper");
    const route_planner planner("osect.db", user_db.db_file, terrain);

    CHECK(expand_or_empty(planner, A + " ? " + C, avoiding(OCEAN_CRUISE_FT)) == A + " " + C);
}

TEST_CASE("terrain above cruise is excluded inside the departure taper")
{
    const test::temporary_tree tree("planner-departure-above", tree_manifest("departure", OCEAN_ZOOM));
    write_tile_at(tree, OCEAN_ZOOM, OCEAN_LAT, east_of_a(3.0), OCEAN_CRUISE_FT + 500.0);
    const elevation_source terrain(tree.path());
    test::tmp_user_db user_db("planner_departure_above");
    const route_planner planner("osect.db", user_db.db_file, terrain);

    CHECK(expand_or_empty(planner, A + " ? " + C, avoiding(OCEAN_CRUISE_FT)) != A + " " + C);
}

TEST_CASE("terrain within the required clearance is excluded outside the tapers")
{
    const test::temporary_tree tree("planner-en-route", tree_manifest("en-route", OCEAN_ZOOM));
    write_tile_at(tree, OCEAN_ZOOM, OCEAN_LAT, east_of_a(12.0), OCEAN_CRUISE_FT - 500.0);
    const elevation_source terrain(tree.path());
    test::tmp_user_db user_db("planner_en_route");
    const route_planner planner("osect.db", user_db.db_file, terrain);

    CHECK(expand_or_empty(planner, A + " ? " + C, avoiding(OCEAN_CRUISE_FT)) != A + " " + C);
}

TEST_CASE("terrain within the required clearance is allowed inside the destination taper")
{
    const test::temporary_tree tree("planner-destination-taper", tree_manifest("destination", OCEAN_ZOOM));
    write_tile_at(tree, OCEAN_ZOOM, OCEAN_LAT, east_of_a(3.0), OCEAN_CRUISE_FT - 500.0);
    const elevation_source terrain(tree.path());
    test::tmp_user_db user_db("planner_destination_taper");
    const route_planner planner("osect.db", user_db.db_file, terrain);

    CHECK(expand_or_empty(planner, C + " ? " + A, avoiding(OCEAN_CRUISE_FT)) == C + " " + A);
}

TEST_CASE("the departure taper continues past a fixed leg into a planned segment")
{
    // Terrain 4 NM past B (8 NM from A). With B as the route start the
    // whole tile lies in the departure taper; after the 4 NM fixed leg
    // A B, the corridor beyond 10 NM from A reaches it at full width
    // and with the required clearance.
    const test::temporary_tree tree("planner-departure-offset", tree_manifest("offset", OCEAN_ZOOM));
    write_tile_at(tree, OCEAN_ZOOM, OCEAN_LAT, east_of_a(8.05), OCEAN_CRUISE_FT - 500.0);
    const elevation_source terrain(tree.path());
    test::tmp_user_db user_db("planner_departure_offset");
    const route_planner planner("osect.db", user_db.db_file, terrain);

    CHECK(expand_or_empty(planner, B + " ? " + C, avoiding(OCEAN_CRUISE_FT)) == B + " " + C);
    CHECK(expand_or_empty(planner, A + " " + B + " ? " + C, avoiding(OCEAN_CRUISE_FT)) != A + " " + B + " " + C);
}

TEST_CASE("the destination taper continues past a fixed leg after a planned segment")
{
    const test::temporary_tree tree("planner-destination-offset", tree_manifest("offset", OCEAN_ZOOM));
    write_tile_at(tree, OCEAN_ZOOM, OCEAN_LAT, east_of_a(8.05), OCEAN_CRUISE_FT - 500.0);
    const elevation_source terrain(tree.path());
    test::tmp_user_db user_db("planner_destination_offset");
    const route_planner planner("osect.db", user_db.db_file, terrain);

    CHECK(expand_or_empty(planner, C + " ? " + B, avoiding(OCEAN_CRUISE_FT)) == C + " " + B);
    CHECK(expand_or_empty(planner, C + " ? " + B + " " + A, avoiding(OCEAN_CRUISE_FT)) != C + " " + B + " " + A);
}

TEST_CASE("terrain avoidance needs a cruise altitude to plan a sigil")
{
    CHECK_THROWS_WITH_AS(ridge_planner().expand_sigils("KSMF ? KBFL", avoiding(std::nullopt)),
                         "terrain avoidance needs a cruise altitude", route_parse_error);
}

TEST_CASE("terrain avoidance without a cruise altitude leaves a route with no sigil alone")
{
    CHECK(ridge_planner().expand_sigils("KSMF KBFL", avoiding(std::nullopt)).text == "KSMF KBFL");
}

TEST_CASE("terrain avoidance needs terrain data to plan a sigil")
{
    const elevation_source terrain("missing-terrain-tree");
    test::tmp_user_db user_db("planner_no_terrain");
    const route_planner planner("osect.db", user_db.db_file, terrain);
    CHECK_THROWS_WITH_AS(planner.expand_sigils(A + " ? " + C, avoiding(OCEAN_CRUISE_FT)),
                         "terrain avoidance needs terrain data", route_parse_error);
}

TEST_CASE("a planned leg outside the terrain coverage is crossed and reported unchecked")
{
    // Coverage starts 0.1 degree (4.9 NM) east of A.
    const test::temporary_tree tree("planner-coverage",
                                    tree_manifest("coverage", OCEAN_ZOOM, "[-123.9, 35, -120, 37]"));
    const elevation_source terrain(tree.path());
    test::tmp_user_db user_db("planner_coverage");
    const route_planner planner("osect.db", user_db.db_file, terrain);

    const auto expansion = planner.expand_sigils(A + " ? " + C, avoiding(OCEAN_CRUISE_FT));
    CHECK(expansion.text == A + " " + C);
    CHECK(expansion.terrain_unchecked_nm > 4.0);
    CHECK(expansion.terrain_unchecked_nm < 12.0);
}

TEST_CASE("a planned leg inside the terrain coverage reports nothing unchecked")
{
    const test::temporary_tree tree("planner-covered", tree_manifest("covered", OCEAN_ZOOM));
    const elevation_source terrain(tree.path());
    test::tmp_user_db user_db("planner_covered");
    const route_planner planner("osect.db", user_db.db_file, terrain);

    CHECK(planner.expand_sigils(A + " ? " + C, avoiding(OCEAN_CRUISE_FT)).terrain_unchecked_nm == 0.0);
}

TEST_CASE("a plan without terrain avoidance reports nothing unchecked")
{
    const test::temporary_tree tree("planner-unchecked-off",
                                    tree_manifest("coverage", OCEAN_ZOOM, "[-123.9, 35, -120, 37]"));
    const elevation_source terrain(tree.path());
    test::tmp_user_db user_db("planner_unchecked_off");
    const route_planner planner("osect.db", user_db.db_file, terrain);

    route_planner::options opts;
    opts.cruise_altitude_ft = OCEAN_CRUISE_FT;
    CHECK(planner.expand_sigils(A + " ? " + C, opts).terrain_unchecked_nm == 0.0);
}
