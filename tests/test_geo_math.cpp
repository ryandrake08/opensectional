#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "geo_math.hpp"

#include <cmath>

using namespace nasrbrowse;

namespace
{
    // WGS84 semi-major axis, nautical miles. Mirrors the constant in
    // geo_math.cpp so tests remain decoupled from internal changes.
    constexpr double EARTH_RADIUS_NM = 6378137.0 / 1852.0;

    // Great-circle distance in nautical miles (haversine).
    double haversine_nm(double lat1, double lon1, double lat2, double lon2)
    {
        double rlat1 = lat1 * M_PI / 180.0;
        double rlat2 = lat2 * M_PI / 180.0;
        double dlat = rlat2 - rlat1;
        double dlon = (lon2 - lon1) * M_PI / 180.0;
        double a = std::sin(dlat * 0.5) * std::sin(dlat * 0.5)
                 + std::cos(rlat1) * std::cos(rlat2)
                   * std::sin(dlon * 0.5) * std::sin(dlon * 0.5);
        return 2.0 * std::atan2(std::sqrt(a), std::sqrt(1.0 - a)) * EARTH_RADIUS_NM;
    }
}

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
        double d = haversine_nm(center_lat, center_lon, p.lat, p.lon);
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
    const double dist_nm = haversine_nm(33.94, -118.40, 40.64, -73.78);
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
        double d = haversine_nm(pts[i-1].lat, pts[i-1].lon,
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
