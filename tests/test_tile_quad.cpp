#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "map_view.hpp"
#include "tile_quad.hpp"

TEST_CASE("tile_quad spans the tile's Web Mercator bounds with two triangles")
{
    const osect::tile_key key{1, 0, 0};
    const auto bounds = osect::tile_bounds_meters(key.x, key.y, key.z);
    const auto x0 = static_cast<float>(bounds.x_min);
    const auto x1 = static_cast<float>(bounds.x_max);
    const auto y0 = static_cast<float>(bounds.y_min);
    const auto y1 = static_cast<float>(bounds.y_max);

    const auto quad = osect::tile_quad(key, 0.25F, 0.5F, 0.75F, 1.0F);

    // Texture v runs north to south, so the south edge (y0) samples v1.
    CHECK(quad[0].x == x0);
    CHECK(quad[0].y == y0);
    CHECK(quad[0].s == 0.25F);
    CHECK(quad[0].t == 1.0F);
    CHECK(quad[1].x == x1);
    CHECK(quad[1].y == y0);
    CHECK(quad[1].s == 0.75F);
    CHECK(quad[1].t == 1.0F);
    CHECK(quad[2].x == x0);
    CHECK(quad[2].y == y1);
    CHECK(quad[2].s == 0.25F);
    CHECK(quad[2].t == 0.5F);
    CHECK(quad[5].x == x1);
    CHECK(quad[5].y == y1);
    CHECK(quad[5].s == 0.75F);
    CHECK(quad[5].t == 0.5F);
}

TEST_CASE("tile_quad repeats the shared corners of its two triangles")
{
    const auto quad = osect::tile_quad({2, 1, 3}, 0.0F, 0.0F, 1.0F, 1.0F);

    CHECK(quad[3].x == quad[2].x);
    CHECK(quad[3].y == quad[2].y);
    CHECK(quad[4].x == quad[1].x);
    CHECK(quad[4].y == quad[1].y);
}

TEST_CASE("tile_quad vertices are opaque white at depth 0")
{
    for(const auto& v : osect::tile_quad({0, 0, 0}, 0.0F, 0.0F, 1.0F, 1.0F))
    {
        CHECK(v.r == 255);
        CHECK(v.g == 255);
        CHECK(v.b == 255);
        CHECK(v.a == 255);
        CHECK(v.z == 0.0F);
    }
}
