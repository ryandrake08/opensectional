#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "tile_cache.hpp"

#include <memory>
#include <vector>

namespace
{
    struct resource
    {
        int value;
    };
}

TEST_CASE("tile_cache requests visible tiles and nearby zoom levels once per range")
{
    osect::tile_cache<resource> cache(3, 2);
    std::vector<osect::tile_key> requested;
    int reset_count = 0;
    const auto request = [&requested](const osect::tile_key& key) { requested.push_back(key); };
    const auto reset = [&reset_count] { reset_count++; };

    CHECK(cache.update(-osect::HALF_CIRCUMFERENCE, -osect::HALF_CIRCUMFERENCE, 0.0, 0.0, 256, reset, request));
    CHECK(cache.visible_tiles().size() == 2);
    CHECK((cache.visible_tiles()[0] == osect::tile_key{1, 0, 1}));
    CHECK((cache.visible_tiles()[1] == osect::tile_key{1, 1, 1}));
    CHECK_FALSE(requested.empty());
    CHECK(reset_count == 1);
    CHECK_FALSE(cache.update(-osect::HALF_CIRCUMFERENCE, -osect::HALF_CIRCUMFERENCE, 0.0, 0.0, 256, reset, request));
    CHECK(reset_count == 1);
}

TEST_CASE("tile_cache resolves a wrapped ancestor with its texture sub-region")
{
    osect::tile_cache<resource> cache(3, 2);
    auto parent = std::make_shared<resource>();
    cache.put({1, 1, 0}, parent);

    osect::tile_cache<resource>::fallback fallback{};
    REQUIRE(cache.find_ancestor({3, -1, 1}, fallback));
    CHECK((fallback.key == osect::tile_key{3, -1, 1}));
    CHECK(fallback.resource == parent);
    CHECK(fallback.u0 == 0.75F);
    CHECK(fallback.v0 == 0.25F);
    CHECK(fallback.u1 == 1.0F);
    CHECK(fallback.v1 == 0.5F);
}

TEST_CASE("tile_cache evicts the least recently used resource")
{
    osect::tile_cache<resource> cache(3, 1);
    cache.put({0, 0, 0}, std::make_shared<resource>());
    auto retained = std::make_shared<resource>();
    cache.put({1, 0, 0}, retained);

    CHECK_FALSE(cache.find({0, 0, 0}));
    CHECK(cache.find({1, 0, 0}) == retained);
}
