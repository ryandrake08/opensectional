#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "geo_math.hpp"

#include <cmath>
#include <vector>

using namespace osect;

TEST_CASE("geodesic_circle returns n+1 points (closed ring)")
{
    auto pts = geodesic_circle(37.0, -122.0, 10.0, 48);
    CHECK(pts.size() == 49);

    // First and last points coincide (same bearing, 0 == 2π)
    CHECK(std::abs(pts.front().lat - pts.back().lat) < 1e-9);
    CHECK(std::abs(pts.front().lon - pts.back().lon) < 1e-9);
}

TEST_CASE("geodesic_circle points all lie at the requested radius")
{
    const double center_lat = 37.0;
    const double center_lon = -122.0;
    const double radius_nm = 25.0;

    auto pts = geodesic_circle(center_lat, center_lon, radius_nm);
    for(const auto& p : pts)
    {
        double d = haversine_distance_nm(center_lat, center_lon, p.lat, p.lon);
        CHECK(std::abs(d - radius_nm) < 0.05);    // < 0.2% error
    }
}

TEST_CASE("geodesic_circle honors custom segment count")
{
    auto pts = geodesic_circle(0.0, 0.0, 5.0, 8);
    CHECK(pts.size() == 9);
}

TEST_CASE("geodesic_interpolate returns endpoints unchanged for short arcs")
{
    // Under threshold — code returns `{{lat1, lon1}, {lat2, lon2}}` with
    // no arithmetic, so values must be bit-identical to the inputs.
    auto pts = geodesic_interpolate(37.0, -122.0, 37.1, -122.1, 50.0);
    REQUIRE(pts.size() == 2);
    CHECK(pts[0].lat == 37.0);
    CHECK(pts[0].lon == -122.0);
    CHECK(pts[1].lat == 37.1);
    CHECK(pts[1].lon == -122.1);
}

TEST_CASE("geodesic_interpolate subdivides long arcs")
{
    // LAX → JFK. Derive the expected segment count from the great-circle
    // distance so the test stays independent of the specific route.
    const double max_seg_nm = 50.0;
    const double dist_nm = haversine_distance_nm(33.94, -118.40, 40.64, -73.78);
    const size_t expected_segments = static_cast<size_t>(std::ceil(dist_nm / max_seg_nm));
    REQUIRE(expected_segments > 1);

    auto pts = geodesic_interpolate(33.94, -118.40, 40.64, -73.78, max_seg_nm);
    REQUIRE(pts.size() == expected_segments + 1);

    // Endpoints preserved
    CHECK(pts.front().lat == doctest::Approx(33.94));
    CHECK(pts.front().lon == doctest::Approx(-118.40));
    CHECK(pts.back().lat == doctest::Approx(40.64));
    CHECK(pts.back().lon == doctest::Approx(-73.78));

    // No segment exceeds threshold (with small numerical slack)
    for(size_t i = 1; i < pts.size(); ++i)
    {
        double d = haversine_distance_nm(pts[i-1].lat, pts[i-1].lon,
                                pts[i].lat, pts[i].lon);
        CHECK(d <= max_seg_nm + 0.5);
    }
}

TEST_CASE("geodesic_interpolate keeps longitude continuous across antimeridian")
{
    // From just east of the dateline to just west of it via the shorter arc
    auto pts = geodesic_interpolate(10.0, 170.0, 10.0, -170.0, 50.0);
    REQUIRE(pts.size() >= 3);

    // The path should cross the dateline using continuous longitudes —
    // i.e. lon either keeps increasing past 180 or decreasing past -180,
    // but no adjacent pair should jump by > 180 degrees.
    for(size_t i = 1; i < pts.size(); ++i)
    {
        double dlon = std::abs(pts[i].lon - pts[i-1].lon);
        CHECK(dlon < 180.0);
    }
}

TEST_CASE("geodesic_circle keeps longitude continuous when crossing antimeridian")
{
    // Center near the dateline with a large radius so the ring wraps
    auto pts = geodesic_circle(0.0, 179.5, 200.0, 48);
    REQUIRE(pts.size() == 49);

    for(size_t i = 1; i < pts.size(); ++i)
    {
        double dlon = std::abs(pts[i].lon - pts[i-1].lon);
        CHECK(dlon < 180.0);
    }
}

TEST_CASE("nearest_to returns the closest candidate")
{
    std::vector<airspace_point> pts = {{40.0, -100.0}, {37.0, -122.0}, {25.0, -80.0}};

    // Query near San Francisco — the second point should win.
    auto it = nearest_to(pts, 37.5, -122.3);
    REQUIRE(it != pts.end());
    // Pass-through of the stored literal — exact.
    CHECK(it->lat == 37.0);
    CHECK(it->lon == -122.0);
}

TEST_CASE("nearest_to returns end() for an empty range")
{
    std::vector<airspace_point> pts;
    CHECK(nearest_to(pts, 0.0, 0.0) == pts.end());
}

TEST_CASE("true_course_deg: cardinal directions")
{
    // bearing via atan2, scaled by 180/M_PI (non-power-of-2 divide)
    CHECK(true_course_deg(0.0, 0.0, 1.0, 0.0) == doctest::Approx(0.0));   // due north
    CHECK(true_course_deg(0.0, 0.0, 0.0, 1.0) == doctest::Approx(90.0));  // due east
    CHECK(true_course_deg(0.0, 0.0, -1.0, 0.0) == doctest::Approx(180.0)); // due south
    CHECK(true_course_deg(0.0, 0.0, 0.0, -1.0) == doctest::Approx(270.0)); // due west
}

TEST_CASE("cross_track_nm: a point on the path has zero cross-track")
{
    // A, B and P all on the equator → P lies on the great circle.
    CHECK(cross_track_nm(0.0, 0.0, 0.0, 10.0, 0.0, 5.0) == doctest::Approx(0.0));
}

TEST_CASE("cross_track_nm: sign distinguishes the two sides of the path")
{
    // Equatorial path heading east; points equally north and south of it.
    auto north = cross_track_nm(0.0, 0.0, 0.0, 10.0, 1.0, 5.0);
    auto south = cross_track_nm(0.0, 0.0, 0.0, 10.0, -1.0, 5.0);
    CHECK(north * south < 0.0);
    CHECK(std::abs(north) == doctest::Approx(std::abs(south)));
}

TEST_CASE("equirectangular_distance_nm: cardinal spans at the equator")
{
    // 1° of latitude = 60 NM; the longitude term multiplies by zero.
    CHECK(equirectangular_distance_nm(0.0, 0.0, 1.0, 0.0) == 60.0);
    // 1° of longitude at the equator: cos(0) is exactly 1.
    CHECK(equirectangular_distance_nm(0.0, 0.0, 0.0, 1.0) == 60.0);
}

TEST_CASE("equirectangular_distance_nm: longitude shrinks with latitude")
{
    // At 60° latitude a degree of longitude spans cos(60°) ≈ 0.5 as far.
    CHECK(equirectangular_distance_nm(60.0, 0.0, 60.0, 1.0) == doctest::Approx(30.0));
}

TEST_CASE("equirectangular_distance_nm is symmetric in its two points")
{
    // Midpoint latitude and squared deltas make the two orders bit-identical.
    auto ab = equirectangular_distance_nm(34.0, -118.0, 40.0, -73.0);
    auto ba = equirectangular_distance_nm(40.0, -73.0, 34.0, -118.0);
    CHECK(ab == ba);
}

TEST_CASE("nm_to_deg_lon converts and clamps at the poles")
{
    // 60 NM = 1° of longitude at the equator (cos(0) is exactly 1).
    CHECK(nm_to_deg_lon(60.0, 0.0) == 1.0);
    // cos(60°) ≈ 0.5, so 30 NM ≈ 1°.
    CHECK(nm_to_deg_lon(30.0, 60.0) == doctest::Approx(1.0));
    // At the pole the cosine vanishes; the result clamps to a hemisphere.
    CHECK(nm_to_deg_lon(60.0, 90.0) == 180.0);
}

TEST_CASE("bbox_around spans radius_nm in each direction")
{
    // 60 NM = 1° of latitude and, at the equator, 1° of longitude.
    auto box = bbox_around(0.0, 0.0, 60.0);
    CHECK(box.lat_min == -1.0);
    CHECK(box.lat_max == 1.0);
    CHECK(box.lon_min == -1.0);
    CHECK(box.lon_max == 1.0);
}
