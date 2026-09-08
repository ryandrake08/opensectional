#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "elevation_address.hpp"

TEST_CASE("Elevation address includes the tile skirt")
{
    const osect::elevation_address address = osect::address_elevation(0.0, 0.0, 0, 256, 2);

    CHECK(address.key == osect::tile_key{0, 0, 0});
    CHECK(address.pixel_x == 129.5);
    CHECK(address.pixel_y == 129.5);
}

TEST_CASE("Elevation address selects the containing tile")
{
    const osect::elevation_address address = osect::address_elevation(45.0, -90.0, 1, 256, 2);

    CHECK(address.key == osect::tile_key{1, 0, 0});
    CHECK(address.pixel_x > 1.5);
    CHECK(address.pixel_x < 257.5);
    CHECK(address.pixel_y > 1.5);
    CHECK(address.pixel_y < 257.5);
}

TEST_CASE("Elevation address wraps longitude at the antimeridian")
{
    const osect::elevation_address west = osect::address_elevation(0.0, -180.0, 2, 256, 2);
    const osect::elevation_address east = osect::address_elevation(0.0, 180.0, 2, 256, 2);
    const osect::elevation_address near_east = osect::address_elevation(0.0, 179.9, 2, 256, 2);

    CHECK(west.key == osect::tile_key{2, 0, 2});
    CHECK(east.key == west.key);
    CHECK(near_east.key == osect::tile_key{2, 3, 2});
}

TEST_CASE("Elevation address clamps latitude to Web Mercator")
{
    const osect::elevation_address north = osect::address_elevation(90.0, 0.0, 2, 256, 2);
    const osect::elevation_address south = osect::address_elevation(-90.0, 0.0, 2, 256, 2);

    CHECK(north.key.y == 0);
    CHECK(south.key.y == 3);
    CHECK(north.pixel_y == 1.5);
    CHECK(south.pixel_y == doctest::Approx(257.5)); // clamps to the last pixel below the Mercator edge
}
