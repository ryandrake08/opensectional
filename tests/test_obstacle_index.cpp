#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "nasr_database.hpp"
#include "obstacle_index.hpp"

#include <algorithm>
#include <initializer_list>

namespace
{
    const osect::nasr_database& test_db()
    {
        static const osect::nasr_database db("osect.db");
        return db;
    }

    // The tallest obstacle the full DOF query finds across `boxes`.
    std::optional<double> queried_maximum_ft(std::initializer_list<osect::geo_bbox> boxes)
    {
        std::optional<double> maximum;
        for(const auto& box : boxes)
        {
            for(const auto& obstacle : test_db().query_obstacles(box))
            {
                maximum = std::max(maximum.value_or(obstacle.amsl_ht), static_cast<double>(obstacle.amsl_ht));
            }
        }
        return maximum;
    }
}

TEST_CASE("obstacle index finds the tallest obstacle in a box within one cell")
{
    osect::obstacle_index index(test_db());
    const osect::geo_bbox box{-122.4, 37.6, -122.2, 37.8};
    REQUIRE(queried_maximum_ft({box}));
    CHECK(index.maximum_ft(box) == queried_maximum_ft({box}));
}

TEST_CASE("obstacle index finds the tallest obstacle in a box spanning several cells")
{
    osect::obstacle_index index(test_db());
    const osect::geo_bbox box{-122.7, 36.6, -120.3, 38.4};
    CHECK(index.maximum_ft(box) == queried_maximum_ft({box}));
}

TEST_CASE("obstacle index reports no obstacle in an empty box")
{
    osect::obstacle_index index(test_db());
    const osect::geo_bbox box{-130.0, 30.0, -129.5, 30.5};
    REQUIRE_FALSE(queried_maximum_ft({box}));
    CHECK_FALSE(index.maximum_ft(box));
}

TEST_CASE("obstacle index matches obstacles whose longitude is a whole turn away")
{
    osect::obstacle_index index(test_db());
    const osect::geo_bbox box{-176.8, 51.7, -176.5, 52.0};
    REQUIRE(queried_maximum_ft({box}));
    CHECK(index.maximum_ft({box.lon_min + 360.0, box.lat_min, box.lon_max + 360.0, box.lat_max}) ==
          queried_maximum_ft({box}));
    CHECK(index.maximum_ft({box.lon_min - 360.0, box.lat_min, box.lon_max - 360.0, box.lat_max}) ==
          queried_maximum_ft({box}));
}

TEST_CASE("obstacle index searches both sides of the antimeridian")
{
    osect::obstacle_index index(test_db());
    // Adak, at about 176.6 W, lies inside a box from 170 E to 176.5 W.
    const osect::geo_bbox box{170.0, 51.7, 183.5, 52.0};
    const auto expected = queried_maximum_ft({{170.0, 51.7, 180.0, 52.0}, {-180.0, 51.7, -176.5, 52.0}});
    REQUIRE(expected);
    CHECK(index.maximum_ft(box) == expected);
}
