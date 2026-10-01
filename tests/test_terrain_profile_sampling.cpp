#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "elevation_address.hpp"
#include "elevation_source.hpp"
#include "elevation_test_tree.hpp"
#include "flight_route.hpp"
#include "geo_math.hpp"
#include "nasr_database.hpp"
#include "terrain_profile.hpp"

#include <algorithm>
#include <cmath>

namespace
{
    const osect::nasr_database& test_db()
    {
        static const osect::nasr_database db("osect.db");
        return db;
    }

    constexpr double HUNDRED_METRES_FT = 328.0839895013123;

    // Writes 1x1-pixel tiles at `zoom` covering columns [x_first, x_last]
    // and rows [y_first, y_last]. The tile containing `peak`, when given,
    // is 100 m; all others are 0 m.
    void write_tiles(const std::filesystem::path& tree, int zoom, int x_first, int x_last, int y_first, int y_last,
                     std::optional<osect::tile_key> peak = std::nullopt)
    {
        for(int x = x_first; x <= x_last; ++x)
        {
            for(int y = y_first; y <= y_last; ++y)
            {
                const bool high = peak && peak->x == x && peak->y == y;
                osect::test::write_elevation_tile(
                    tree / std::to_string(zoom) / std::to_string(x) / (std::to_string(y) + ".png"), high);
            }
        }
    }
}

TEST_CASE("terrain profile keeps stations within the maximum interval without terrain data")
{
    const auto airports = test_db().lookup_airports("O61");
    const auto destination = test_db().lookup_airports("KMER");
    REQUIRE(airports.size() == 1);
    REQUIRE(destination.size() == 1);

    const std::vector<osect::route_waypoint> waypoints{
        {osect::waypoint_kind::airport, "O61", airports.front().lat, airports.front().lon},
        {osect::waypoint_kind::airport, "KMER", destination.front().lat, destination.front().lon},
    };
    const osect::elevation_source terrain("missing-terrain-tree");
    const osect::terrain_profile profile = osect::build_terrain_profile(
        waypoints, terrain, test_db(), 10000.0, osect::terrain_profile_gradients{300.0, 318.0}, 4.0, false);

    const double distance_nm = osect::haversine_distance_nm(
        airports.front().lat, airports.front().lon, destination.front().lat, destination.front().lon);
    for(std::size_t i = 1; i < profile.samples.size(); ++i)
    {
        CHECK(profile.samples[i].distance_nm - profile.samples[i - 1].distance_nm <=
              osect::TERRAIN_PROFILE_MAX_SAMPLE_INTERVAL_NM);
    }

    REQUIRE(profile.samples.front().centreline_elevation_ft);
    CHECK(*profile.samples.front().centreline_elevation_ft == airports.front().elev);
    REQUIRE(profile.samples.back().centreline_elevation_ft);
    CHECK(*profile.samples.back().centreline_elevation_ft == destination.front().elev);
    CHECK(profile.samples.front().distance_nm == 0.0);
    CHECK(profile.samples.back().distance_nm == doctest::Approx(distance_nm)); // great-circle interpolation
    CHECK(profile.samples.front().aircraft_altitude_ft == airports.front().elev);
    CHECK(profile.samples.back().aircraft_altitude_ft == destination.front().elev);
    CHECK(profile.samples.front().corridor_elevation_ft == airports.front().elev);
    CHECK(profile.samples.back().corridor_elevation_ft == destination.front().elev);
    CHECK(std::any_of(profile.samples.begin(), profile.samples.end(), [](const auto& sample)
                      { return sample.aircraft_altitude_ft == 10000.0; }));
}

TEST_CASE("terrain profile omits aircraft trace without a cruise altitude")
{
    const auto airports = test_db().lookup_airports("O61");
    const auto destination = test_db().lookup_airports("KMER");
    REQUIRE(airports.size() == 1);
    REQUIRE(destination.size() == 1);

    const std::vector<osect::route_waypoint> waypoints{
        {osect::waypoint_kind::airport, "O61", airports.front().lat, airports.front().lon},
        {osect::waypoint_kind::airport, "KMER", destination.front().lat, destination.front().lon},
    };
    const osect::elevation_source terrain("missing-terrain-tree");
    const osect::terrain_profile profile = osect::build_terrain_profile(
        waypoints, terrain, test_db(), std::nullopt, osect::terrain_profile_gradients{}, 4.0, false);

    for(std::size_t i = 0; i < profile.samples.size(); ++i)
    {
        const auto& sample = profile.samples[i];
        CHECK_FALSE(sample.aircraft_altitude_ft);
        if(i == 0 || i + 1 == profile.samples.size())
        {
            CHECK(sample.corridor_elevation_ft == sample.centreline_elevation_ft);
        }
        else
        {
            CHECK_FALSE(sample.corridor_elevation_ft);
        }
    }
}

TEST_CASE("terrain profile starts descent from the arrival gradient")
{
    const auto departure = test_db().lookup_airports("O61");
    const auto arrival = test_db().lookup_airports("KPMD");
    REQUIRE(departure.size() == 1);
    REQUIRE(arrival.size() == 1);

    const std::vector<osect::route_waypoint> waypoints{
        {osect::waypoint_kind::airport, "O61", departure.front().lat, departure.front().lon},
        {osect::waypoint_kind::airport, "KPMD", arrival.front().lat, arrival.front().lon},
    };
    constexpr double cruise_altitude_ft = 5500.0;
    constexpr double descent_gradient_ft_per_nm = 318.0;
    const osect::elevation_source terrain("missing-terrain-tree");
    const osect::terrain_profile profile = osect::build_terrain_profile(
        waypoints, terrain, test_db(), cruise_altitude_ft, {300.0, descent_gradient_ft_per_nm}, 4.0, false);

    const double descent_start_nm = profile.samples.back().distance_nm -
                                    (cruise_altitude_ft - arrival.front().elev) / descent_gradient_ft_per_nm;
    const double climb_end_nm = (cruise_altitude_ft - departure.front().elev) / 300.0;
    CHECK_FALSE(std::any_of(profile.samples.begin(), profile.samples.end(), [&](const auto& sample)
                            {
                                return sample.distance_nm > climb_end_nm && sample.distance_nm <= descent_start_nm &&
                                       sample.aircraft_altitude_ft && *sample.aircraft_altitude_ft != cruise_altitude_ft;
                            }));
    const auto first_descent = std::find_if(profile.samples.begin(), profile.samples.end(), [&](const auto& sample)
                                            {
                                                return sample.distance_nm > descent_start_nm && sample.aircraft_altitude_ft &&
                                                       *sample.aircraft_altitude_ft < cruise_altitude_ft;
                                            });
    REQUIRE(first_descent != profile.samples.end());
    CHECK(first_descent->distance_nm > descent_start_nm);
    CHECK((first_descent - 1)->distance_nm <= descent_start_nm);
}

TEST_CASE("terrain profile clips the aircraft trace below cruise on a short route")
{
    const auto departure = test_db().lookup_airports("O61");
    const auto arrival = test_db().lookup_airports("08CA");
    REQUIRE(departure.size() == 1);
    REQUIRE(arrival.size() == 1);

    const std::vector<osect::route_waypoint> waypoints{
        {osect::waypoint_kind::airport, "O61", departure.front().lat, departure.front().lon},
        {osect::waypoint_kind::airport, "08CA", arrival.front().lat, arrival.front().lon},
    };
    const osect::elevation_source terrain("missing-terrain-tree");
    const osect::terrain_profile profile = osect::build_terrain_profile(
        waypoints, terrain, test_db(), 10000.0, osect::terrain_profile_gradients{300.0, 318.0});

    CHECK(profile.samples.size() >= osect::TERRAIN_PROFILE_MIN_SAMPLES);
    for(const auto& sample : profile.samples)
    {
        REQUIRE(sample.aircraft_altitude_ft);
        CHECK(*sample.aircraft_altitude_ft < 10000.0);
    }
}

TEST_CASE("terrain profile rejects a short route with no climb-descent intersection")
{
    const auto departure = test_db().lookup_airports("O61");
    const auto arrival = test_db().lookup_airports("KMER");
    REQUIRE(departure.size() == 1);
    REQUIRE(arrival.size() == 1);
    REQUIRE(departure.front().elev != arrival.front().elev);

    const std::vector<osect::route_waypoint> waypoints{
        {osect::waypoint_kind::airport, "O61", departure.front().lat, departure.front().lon},
        {osect::waypoint_kind::airport, "KMER", departure.front().lat, departure.front().lon},
    };
    const osect::elevation_source terrain("missing-terrain-tree");
    CHECK_THROWS_WITH(osect::build_terrain_profile(waypoints, terrain, test_db(), 10000.0,
                                                    osect::terrain_profile_gradients{300.0, 318.0}),
                      "route is too short for the configured climb and descent gradients");
}

TEST_CASE("terrain profile derives maxima, MSA, and clearance spans")
{
    const auto departure = test_db().lookup_airports("O61");
    const auto arrival = test_db().lookup_airports("KMER");
    REQUIRE(departure.size() == 1);
    REQUIRE(arrival.size() == 1);

    const std::vector<osect::route_waypoint> waypoints{
        {osect::waypoint_kind::airport, "O61", departure.front().lat, departure.front().lon},
        {osect::waypoint_kind::airport, "KMER", arrival.front().lat, arrival.front().lon},
    };
    const osect::elevation_source terrain("missing-terrain-tree");
    const osect::terrain_profile profile = osect::build_terrain_profile(
        waypoints, terrain, test_db(), 10000.0, osect::terrain_profile_gradients{300.0, 318.0}, 4.0, true, 100000.0);

    REQUIRE(profile.maximum_elevation_ft);
    CHECK(std::all_of(profile.samples.begin() + 1, profile.samples.end() - 1, [](const auto& sample)
                      { return !sample.corridor_elevation_ft || sample.corridor_from_obstacle; }));
    REQUIRE(profile.legs.size() == 1);
    REQUIRE(profile.legs.front().maximum_elevation_ft);
    REQUIRE(profile.legs.front().msa_ft);
    CHECK(*profile.legs.front().msa_ft == *profile.legs.front().maximum_elevation_ft + 100000.0);
    CHECK_FALSE(profile.clearance_spans.empty());
}

TEST_CASE("terrain profile keeps flat terrain at the maximum station interval")
{
    constexpr int zoom = 8;
    osect::test::temporary_tree tree("osect-profile-flat", osect::test::manifest("flat", 1, 0, zoom, zoom, 1.0));
    const auto first = osect::address_elevation(0.5, 0.2, zoom, 1, 0).key;
    const auto last = osect::address_elevation(0.5, 5.4, zoom, 1, 0).key;
    write_tiles(tree.path(), zoom, first.x, last.x, first.y - 1, first.y + 1);

    const std::vector<osect::route_waypoint> waypoints{
        {osect::waypoint_kind::latlon, "A", 0.5, 0.2},
        {osect::waypoint_kind::latlon, "B", 0.5, 5.4},
    };
    const osect::elevation_source terrain(tree.path());
    const osect::terrain_profile profile =
        osect::build_terrain_profile(waypoints, terrain, test_db(), std::nullopt, {}, 4.0, false);

    const double distance_nm = osect::haversine_distance_nm(0.5, 0.2, 0.5, 5.4);
    const double terminal_nm = osect::TERRAIN_PROFILE_TERMINAL_CORRIDOR_DISTANCE_NM;
    const double interval_nm = osect::TERRAIN_PROFILE_MAX_SAMPLE_INTERVAL_NM;
    const auto terminal_stations = static_cast<std::size_t>(std::ceil(terminal_nm / interval_nm));
    const auto cruise_stations =
        static_cast<std::size_t>(std::ceil((distance_nm - 2.0 * terminal_nm) / interval_nm));
    CHECK(profile.samples.size() == 2 * terminal_stations + cruise_stations + 1);
    for(const auto& sample : profile.samples)
    {
        CHECK(sample.centreline_elevation_ft == 0.0);
    }
}

TEST_CASE("terrain profile refines stations around a narrow peak")
{
    constexpr int zoom = 15;
    constexpr double lat = 0.005;
    constexpr double peak_lon = 2.003;
    osect::test::temporary_tree tree("osect-profile-peak", osect::test::manifest("peak", 1, 0, zoom, zoom, 1.0));
    const auto peak = osect::address_elevation(lat, peak_lon, zoom, 1, 0).key;
    write_tiles(tree.path(), zoom, peak.x - 2, peak.x + 2, peak.y - 1, peak.y + 1, peak);

    const std::vector<osect::route_waypoint> waypoints{
        {osect::waypoint_kind::latlon, "A", lat, 0.0},
        {osect::waypoint_kind::latlon, "B", lat, 4.0},
    };
    const osect::elevation_source terrain(tree.path());
    const osect::terrain_profile profile =
        osect::build_terrain_profile(waypoints, terrain, test_db(), std::nullopt, {}, 4.0, false);

    const auto highest = std::max_element(profile.samples.begin(), profile.samples.end(),
                                          [](const auto& a, const auto& b)
                                          { return a.centreline_elevation_ft < b.centreline_elevation_ft; });
    REQUIRE(highest->centreline_elevation_ft);
    CHECK(*highest->centreline_elevation_ft == doctest::Approx(HUNDRED_METRES_FT));

    // The corridor line between the stations around the peak stays at or
    // above it.
    const double peak_nm = osect::haversine_distance_nm(lat, 0.0, lat, peak_lon);
    const auto after = std::find_if(profile.samples.begin(), profile.samples.end(),
                                    [&](const auto& sample) { return sample.distance_nm > peak_nm; });
    REQUIRE(after != profile.samples.begin());
    REQUIRE(after != profile.samples.end());
    const auto before = after - 1;
    REQUIRE(before->corridor_elevation_ft);
    REQUIRE(after->corridor_elevation_ft);
    CHECK(*before->corridor_elevation_ft == doctest::Approx(HUNDRED_METRES_FT));
    CHECK(*after->corridor_elevation_ft == doctest::Approx(HUNDRED_METRES_FT));

    // Stations within the tiles are spaced no wider than the minimum
    // interval; stations away from the data stay at the maximum interval.
    CHECK(after->distance_nm - before->distance_nm <= osect::TERRAIN_PROFILE_SAMPLE_INTERVAL_NM);
    const double distance_nm = osect::haversine_distance_nm(lat, 0.0, lat, 4.0);
    CHECK(profile.samples.size() < static_cast<std::size_t>(distance_nm / osect::TERRAIN_PROFILE_SAMPLE_INTERVAL_NM));
}

TEST_CASE("terrain profile includes obstacles in the corridor and excludes those beside it")
{
    // Starting in northern California, move to the tallest obstacle within
    // half a degree until none nearby is taller. Both legs' corridors lie
    // within half a degree of it.
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

    // A 25 NM north-east leg centred on the obstacle, and the same leg
    // moved 6 NM to its north-west, beyond the 4 NM corridor half-width.
    const double nm_lat = 1.0 / 60.0;
    const double nm_lon = 1.0 / (60.0 * std::cos(tallest.lat * M_PI / 180.0));
    const auto leg = [&](double offset_nm)
    {
        const double lat = tallest.lat + offset_nm * std::sqrt(0.5) * nm_lat;
        const double lon = tallest.lon - offset_nm * std::sqrt(0.5) * nm_lon;
        const double half_nm = 12.5 * std::sqrt(0.5);
        return std::vector<osect::route_waypoint>{
            {osect::waypoint_kind::latlon, "A", lat - half_nm * nm_lat, lon - half_nm * nm_lon},
            {osect::waypoint_kind::latlon, "B", lat + half_nm * nm_lat, lon + half_nm * nm_lon},
        };
    };
    const auto highest_corridor = [](const osect::terrain_profile& profile)
    {
        std::optional<double> highest;
        for(const auto& sample : profile.samples)
        {
            if(sample.corridor_elevation_ft && (!highest || *sample.corridor_elevation_ft > *highest))
            {
                highest = sample.corridor_elevation_ft;
            }
        }
        return highest;
    };

    const osect::elevation_source terrain("missing-terrain-tree");
    const auto over = osect::build_terrain_profile(leg(0.0), terrain, test_db(), std::nullopt);
    const auto beside = osect::build_terrain_profile(leg(6.0), terrain, test_db(), std::nullopt);

    CHECK(highest_corridor(over) == static_cast<double>(tallest.amsl_ht));
    const auto beside_highest = highest_corridor(beside);
    CHECK((!beside_highest || *beside_highest < tallest.amsl_ht));
}
