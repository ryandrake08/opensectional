#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "map_view.hpp"

#include <cmath>

using namespace nasrbrowse;

TEST_CASE("HALF_CIRCUMFERENCE matches the Web Mercator reference value")
{
    // Widely-cited Web Mercator half-circumference (EPSG:3857)
    CHECK(HALF_CIRCUMFERENCE == 20037508.342789244);
}

TEST_CASE("longitude ↔ mx round-trips at representative values")
{
    // Round-trip divides by a non-power-of-2 (180.0) in each direction,
    // so bit-equality isn't guaranteed — use Approx.
    for(double lon : {-180.0, -120.0, -45.5, 0.0, 45.5, 120.0, 179.9})
    {
        double mx = lon_to_mx(lon);
        CHECK(mx_to_lon(mx) == doctest::Approx(lon));
    }
}

TEST_CASE("lon_to_mx spans [-HALF_CIRCUMFERENCE, +HALF_CIRCUMFERENCE]")
{
    // ±180 involves division by 180 so isn't guaranteed bit-exact;
    // 0 * anything is exactly 0.
    CHECK(lon_to_mx(-180.0) == doctest::Approx(-HALF_CIRCUMFERENCE));
    CHECK(lon_to_mx(0.0)    == 0.0);
    CHECK(lon_to_mx(180.0)  == doctest::Approx(HALF_CIRCUMFERENCE));
}

TEST_CASE("latitude ↔ my round-trips")
{
    for(double lat : {-85.0, -60.0, -30.0, 0.0, 30.0, 60.0, 85.0})
    {
        double my = lat_to_my(lat);
        CHECK(my_to_lat(my) == doctest::Approx(lat));
    }
}

TEST_CASE("lat_to_my(0) == 0 (equator)")
{
    CHECK(lat_to_my(0.0) == doctest::Approx(0.0));
}

TEST_CASE("lonlat_to_tile at zoom 0 returns (0, 0) for any point in the world")
{
    int tx = -1, ty = -1;
    lonlat_to_tile(0.0, 0.0, 0, tx, ty);
    CHECK(tx == 0);
    CHECK(ty == 0);

    lonlat_to_tile(-120.0, 45.0, 0, tx, ty);
    CHECK(tx == 0);
    CHECK(ty == 0);
}

TEST_CASE("lonlat_to_tile at zoom 1 splits the world into 4 tiles")
{
    int tx, ty;

    // NW quadrant (top-left)
    lonlat_to_tile(-90.0, 45.0, 1, tx, ty);
    CHECK(tx == 0);
    CHECK(ty == 0);

    // NE quadrant (top-right)
    lonlat_to_tile(90.0, 45.0, 1, tx, ty);
    CHECK(tx == 1);
    CHECK(ty == 0);

    // SW quadrant (bottom-left)
    lonlat_to_tile(-90.0, -45.0, 1, tx, ty);
    CHECK(tx == 0);
    CHECK(ty == 1);

    // SE quadrant (bottom-right)
    lonlat_to_tile(90.0, -45.0, 1, tx, ty);
    CHECK(tx == 1);
    CHECK(ty == 1);
}

TEST_CASE("tile_to_lonlat gives the NW corner of the tile")
{
    double lon = 0.0, lat = 0.0;

    // Tile (0,0) at zoom 0 — whole world, NW corner is (-180, 85.05…).
    // lon path is `tx/n * 360 - 180` which is exact for these inputs;
    // lat path goes through atan(sinh(...)) so needs Approx.
    tile_to_lonlat(0, 0, 0, lon, lat);
    CHECK(lon == -180.0);
    CHECK(lat == doctest::Approx(85.05112878).epsilon(1e-5));

    // Tile (1,0) at zoom 1 — NW corner at (0, 85.05…)
    tile_to_lonlat(1, 0, 1, lon, lat);
    CHECK(lon == 0.0);
    CHECK(lat == doctest::Approx(85.05112878).epsilon(1e-5));
}

TEST_CASE("lonlat_to_tile and tile_to_lonlat round-trip tile indices")
{
    int zoom = 10;
    for(int tx : {0, 100, 512, 1023})
    {
        for(int ty : {0, 100, 512, 1023})
        {
            double lon, lat;
            tile_to_lonlat(tx, ty, zoom, lon, lat);

            // Nudge toward tile interior so truncation doesn't land on a boundary
            double nlon = lon + 1e-6;
            double nlat = lat - 1e-6;
            int rtx, rty;
            lonlat_to_tile(nlon, nlat, zoom, rtx, rty);
            CHECK(rtx == tx);
            CHECK(rty == ty);
        }
    }
}

TEST_CASE("tile_bounds_meters spans a full world at zoom 0")
{
    // All arithmetic at zoom 0/1 is exact: /1 or /2 on HALF_CIRCUMFERENCE,
    // plus additions whose true results fit in the mantissa.
    auto [x_min, y_min, x_max, y_max] = tile_bounds_meters(0, 0, 0);
    CHECK(x_min == -HALF_CIRCUMFERENCE);
    CHECK(x_max ==  HALF_CIRCUMFERENCE);
    CHECK(y_min == -HALF_CIRCUMFERENCE);
    CHECK(y_max ==  HALF_CIRCUMFERENCE);
}

TEST_CASE("tile_bounds_meters at zoom 1: all four tiles tile the world")
{
    auto nw = tile_bounds_meters(0, 0, 1);
    auto ne = tile_bounds_meters(1, 0, 1);
    auto sw = tile_bounds_meters(0, 1, 1);
    auto se = tile_bounds_meters(1, 1, 1);

    // Tile (0,0) is NW corner
    CHECK(nw.x_min == -HALF_CIRCUMFERENCE);
    CHECK(nw.y_max ==  HALF_CIRCUMFERENCE);
    CHECK(nw.x_max == 0.0);
    CHECK(nw.y_min == 0.0);

    // NE/SW/SE quadrants have matching seam coordinates
    CHECK(ne.x_min == 0.0);
    CHECK(sw.y_max == 0.0);
    CHECK(se.x_max ==  HALF_CIRCUMFERENCE);
    CHECK(se.y_min == -HALF_CIRCUMFERENCE);
}

TEST_CASE("zoom_level: 256m/pixel at the equator equals zoom 0 for a 256-px viewport")
{
    // All intermediates here cancel exactly — log2(1) is 0 in IEEE 754.
    double z = zoom_level(HALF_CIRCUMFERENCE, 256);
    CHECK(z == 0.0);
}

TEST_CASE("zoom_level: doubling pixels halves half_extent_y for same zoom")
{
    // Both calls reduce to the same intermediate value (meters_per_pixel =
    // 7.8125), so they must produce bit-identical log2 results.
    double z_a = zoom_level(1000.0, 256);
    double z_b = zoom_level(500.0, 128);
    CHECK(z_a == z_b);
}

TEST_CASE("map_view::zoom_to_level recovers the requested integer level")
{
    map_view v;
    v.viewport_width = 800;
    v.viewport_height = 600;

    for(int z : {5, 8, 12, 16})
    {
        v.zoom_to_level(z);
        CHECK(v.zoom_level() == doctest::Approx(static_cast<double>(z)));
    }
}

TEST_CASE("map_view::pan_meters moves the center")
{
    map_view v;
    v.viewport_width = 800;
    v.viewport_height = 600;
    double cx0 = v.center_x;
    double cy0 = v.center_y;

    v.pan_meters(1000.0, -500.0);
    // center_x goes through (x + H) fmod 2H - H in clamp_center, which is
    // not bit-exact; center_y's clamp is a branchless compare/assign.
    CHECK(v.center_x == doctest::Approx(cx0 + 1000.0));
    CHECK(v.center_y == cy0 - 500.0);
}

TEST_CASE("map_view::zoom() shrinks/grows half_extent_y within the clamp range")
{
    map_view v;
    v.viewport_width = 800;
    v.viewport_height = 600;
    v.zoom_to_level(10);
    double h0 = v.half_extent_y;

    // Multiplication by 0.5 and 2.0 is exact in IEEE 754.
    v.zoom(0.5);
    CHECK(v.half_extent_y == h0 * 0.5);

    v.zoom(2.0);
    CHECK(v.half_extent_y == h0);
}
