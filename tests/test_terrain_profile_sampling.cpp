#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "elevation_source.hpp"
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
}

TEST_CASE("terrain profile samples airport route at the profile interval")
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
    REQUIRE(profile.samples.size() == static_cast<std::size_t>(std::ceil(
                                          distance_nm / osect::TERRAIN_PROFILE_SAMPLE_INTERVAL_NM)) +
                                          1);

    REQUIRE(profile.samples.front().centreline_elevation_ft);
    CHECK(*profile.samples.front().centreline_elevation_ft == airports.front().elev);
    REQUIRE(profile.samples.back().centreline_elevation_ft);
    CHECK(*profile.samples.back().centreline_elevation_ft == destination.front().elev);
    CHECK(profile.samples.front().distance_nm == 0.0);
    CHECK(profile.samples.back().distance_nm == doctest::Approx(distance_nm)); // great-circle interpolation
    CHECK(profile.samples.front().aircraft_altitude_ft == airports.front().elev);
    CHECK(profile.samples.back().aircraft_altitude_ft == destination.front().elev);
    CHECK_FALSE(profile.samples.front().corridor_elevation_ft);
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

    for(const auto& sample : profile.samples)
    {
        CHECK_FALSE(sample.aircraft_altitude_ft);
        CHECK_FALSE(sample.corridor_elevation_ft);
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
    CHECK(std::all_of(profile.samples.begin(), profile.samples.end(), [](const auto& sample)
                      { return !sample.corridor_elevation_ft || sample.corridor_from_obstacle; }));
    REQUIRE(profile.legs.size() == 1);
    REQUIRE(profile.legs.front().maximum_elevation_ft);
    REQUIRE(profile.legs.front().msa_ft);
    CHECK(*profile.legs.front().msa_ft == *profile.legs.front().maximum_elevation_ft + 100000.0);
    CHECK_FALSE(profile.clearance_spans.empty());
}
