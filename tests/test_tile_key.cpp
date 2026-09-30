#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "tile_key.hpp"

TEST_CASE("tile_key::wrapped keeps an in-range column")
{
    CHECK((osect::tile_key{3, 5, 2}.wrapped() == osect::tile_key{3, 5, 2}));
}

TEST_CASE("tile_key::wrapped folds columns across the antimeridian")
{
    CHECK((osect::tile_key{3, -1, 2}.wrapped() == osect::tile_key{3, 7, 2}));
    CHECK((osect::tile_key{3, 8, 2}.wrapped() == osect::tile_key{3, 0, 2}));
    CHECK((osect::tile_key{3, -9, 2}.wrapped() == osect::tile_key{3, 7, 2}));
}

TEST_CASE("tile_key::wrapped maps every column to 0 at zoom 0")
{
    CHECK((osect::tile_key{0, 3, 0}.wrapped() == osect::tile_key{0, 0, 0}));
}

TEST_CASE("tile_key::parent halves the wrapped column and row")
{
    CHECK((osect::tile_key{3, 5, 3}.parent() == osect::tile_key{2, 2, 1}));
    CHECK((osect::tile_key{3, -1, 2}.parent() == osect::tile_key{2, 3, 1}));
}

TEST_CASE("tile_file_path builds root/z/x/y.png with the wrapped column")
{
    CHECK(osect::tile_file_path("tiles", {3, -1, 2}) == std::filesystem::path("tiles/3/7/2.png"));
}
