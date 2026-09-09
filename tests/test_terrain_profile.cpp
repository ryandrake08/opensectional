#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "terrain_profile.hpp"

TEST_CASE("terrain profile value types retain profile results")
{
    osect::terrain_profile_sample sample{
        12.5,
        800.0,
        1200.0,
        4500.0,
        true,
    };
    osect::terrain_profile_leg leg{
        1,
        10.0,
        20.0,
        1200.0,
        2200.0,
        true,
    };
    osect::terrain_profile profile{
        {sample},
        {leg},
        1200.0,
        {},
    };
    osect::terrain_profile_gradients gradients{300.0, 318.0};

    CHECK(sample.distance_nm == 12.5);
    REQUIRE(sample.centreline_elevation_ft);
    CHECK(*sample.centreline_elevation_ft == 800.0);
    REQUIRE(sample.corridor_elevation_ft);
    CHECK(*sample.corridor_elevation_ft == 1200.0);
    REQUIRE(sample.aircraft_altitude_ft);
    CHECK(*sample.aircraft_altitude_ft == 4500.0);
    CHECK(sample.has_obstacle);

    CHECK(leg.route_leg_index == 1);
    CHECK(leg.start_distance_nm == 10.0);
    CHECK(leg.end_distance_nm == 20.0);
    REQUIRE(leg.maximum_elevation_ft);
    CHECK(*leg.maximum_elevation_ft == 1200.0);
    REQUIRE(leg.msa_ft);
    CHECK(*leg.msa_ft == 2200.0);
    CHECK(leg.has_obstacle);

    REQUIRE(profile.maximum_elevation_ft);
    CHECK(*profile.maximum_elevation_ft == 1200.0);
    CHECK(gradients.climb_ft_per_nm == 300.0);
    CHECK(gradients.descent_ft_per_nm == 318.0);
}

TEST_CASE("terrain profile defaults represent unavailable derived values")
{
    osect::terrain_profile_sample sample{};
    osect::terrain_profile_leg leg{};
    osect::terrain_profile profile{};

    CHECK_FALSE(sample.centreline_elevation_ft);
    CHECK_FALSE(sample.corridor_elevation_ft);
    CHECK_FALSE(sample.aircraft_altitude_ft);
    CHECK_FALSE(sample.has_obstacle);
    CHECK_FALSE(leg.maximum_elevation_ft);
    CHECK_FALSE(leg.msa_ft);
    CHECK_FALSE(leg.has_obstacle);
    CHECK_FALSE(profile.maximum_elevation_ft);
    CHECK(profile.samples.empty());
    CHECK(profile.legs.empty());
}
