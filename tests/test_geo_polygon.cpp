#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "geo_polygon.hpp"

using namespace osect;

namespace
{
    struct ring
    {
        std::vector<geo_point> points;
        bool is_hole = false;
    };

    // Axis-aligned square ring from (lon0, lat0) to (lon1, lat1).
    std::vector<geo_point> square(double lon0, double lat0, double lon1, double lat1)
    {
        return {{lat0, lon0}, {lat0, lon1}, {lat1, lon1}, {lat1, lon0}};
    }

    // A 10x10 square with a 2x2 hole in the middle.
    const std::vector<ring> donut = {{square(0, 0, 10, 10), false}, {square(4, 4, 6, 6), true}};
}

TEST_CASE("point_in_ring inside and outside")
{
    const auto sq = square(0, 0, 10, 10);
    CHECK(point_in_ring(5, 5, sq));
    CHECK_FALSE(point_in_ring(11, 5, sq));
    CHECK_FALSE(point_in_ring(5, -1, sq));
}

TEST_CASE("point_in_multi_ring excludes holes")
{
    CHECK(point_in_multi_ring(2, 2, donut));
    CHECK_FALSE(point_in_multi_ring(5, 5, donut));
    CHECK_FALSE(point_in_multi_ring(20, 20, donut));
}

TEST_CASE("segments_intersect crossing")
{
    CHECK(segments_intersect({0, 0}, {10, 10}, {0, 10}, {10, 0}));
}

TEST_CASE("segments_intersect disjoint")
{
    CHECK_FALSE(segments_intersect({0, 0}, {1, 1}, {5, 0}, {6, 1}));
}

TEST_CASE("segments_intersect touching at an endpoint")
{
    CHECK(segments_intersect({0, 0}, {5, 5}, {5, 5}, {10, 0}));
}

TEST_CASE("segments_intersect parallel and apart")
{
    CHECK_FALSE(segments_intersect({0, 0}, {0, 10}, {1, 0}, {1, 10}));
}

TEST_CASE("segments_intersect collinear overlap")
{
    CHECK(segments_intersect({0, 0}, {0, 10}, {0, 5}, {0, 15}));
}

TEST_CASE("segments_intersect collinear and apart")
{
    CHECK_FALSE(segments_intersect({0, 0}, {0, 4}, {0, 5}, {0, 15}));
}

TEST_CASE("segment_meets_multi_ring with both ends outside crossing the area")
{
    CHECK(segment_meets_multi_ring({5, -5}, {5, 15}, donut));
}

TEST_CASE("segment_meets_multi_ring with one end inside")
{
    CHECK(segment_meets_multi_ring({2, 2}, {2, 20}, donut));
}

TEST_CASE("segment_meets_multi_ring with the segment entirely inside")
{
    CHECK(segment_meets_multi_ring({1, 1}, {2, 2}, donut));
}

TEST_CASE("segment_meets_multi_ring passing beside the area")
{
    CHECK_FALSE(segment_meets_multi_ring({-5, -1}, {15, -1}, donut));
}

TEST_CASE("segment_meets_multi_ring entirely inside a hole")
{
    CHECK_FALSE(segment_meets_multi_ring({4.5, 4.5}, {5.5, 5.5}, donut));
}
