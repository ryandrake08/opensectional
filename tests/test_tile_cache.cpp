#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "tile_cache.hpp"

#include <memory>
#include <unordered_set>
#include <vector>

namespace
{
    struct resource
    {
        int value;
    };

    // Records requests; keys in failed_keys report as failed.
    struct fake_loader
    {
        std::unordered_set<osect::tile_key> failed_keys;
        std::vector<osect::tile_key> requested;
        int cancels = 0;

        bool failed(const osect::tile_key& key) const
        {
            return failed_keys.count(key) != 0;
        }

        void request(const osect::tile_key& key)
        {
            requested.push_back(key);
        }

        void cancel()
        {
            cancels++;
        }
    };
}

TEST_CASE("tile_cache::update cancels queued loads and requests a new range once")
{
    osect::tile_cache<resource> cache(0, 3, 256, 2);
    fake_loader loader;

    CHECK(cache.update(-osect::HALF_CIRCUMFERENCE, -osect::HALF_CIRCUMFERENCE, 0.0, 0.0, 256, loader));
    CHECK(cache.visible_tiles().size() == 2);
    CHECK((cache.visible_tiles()[0] == osect::tile_key{1, 0, 1}));
    CHECK((cache.visible_tiles()[1] == osect::tile_key{1, 1, 1}));
    CHECK_FALSE(loader.requested.empty());
    CHECK(loader.cancels == 1);
    CHECK_FALSE(cache.update(-osect::HALF_CIRCUMFERENCE, -osect::HALF_CIRCUMFERENCE, 0.0, 0.0, 256, loader));
    CHECK(loader.cancels == 1);
}

TEST_CASE("tile_cache::update re-requests visible tiles, or their nearest unfailed ancestors, when the range is unchanged")
{
    osect::tile_cache<resource> cache(0, 3, 256, 4);
    fake_loader loader;
    loader.failed_keys = {{1, 1, 1}};
    cache.update(-osect::HALF_CIRCUMFERENCE, -osect::HALF_CIRCUMFERENCE, 0.0, 0.0, 256, loader);
    loader.requested.clear();

    CHECK_FALSE(cache.update(-osect::HALF_CIRCUMFERENCE, -osect::HALF_CIRCUMFERENCE, 0.0, 0.0, 256, loader));

    CHECK(loader.requested == std::vector<osect::tile_key>{{1, 0, 1}, {0, 0, 0}});
}

TEST_CASE("tile_cache resolves a wrapped ancestor with its texture sub-region")
{
    osect::tile_cache<resource> cache(0, 3, 256, 2);
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
    osect::tile_cache<resource> cache(0, 3, 256, 1);
    cache.put({0, 0, 0}, std::make_shared<resource>());
    auto retained = std::make_shared<resource>();
    cache.put({1, 0, 0}, retained);

    CHECK_FALSE(cache.find({0, 0, 0}));
    CHECK(cache.find({1, 0, 0}) == retained);
}

TEST_CASE("tile_cache picks the zoom level from its tile size")
{
    osect::tile_cache<resource> cache(0, 3, 512, 2);
    fake_loader loader;

    cache.update(-osect::HALF_CIRCUMFERENCE, -osect::HALF_CIRCUMFERENCE, 0.0, 0.0, 256, loader);

    REQUIRE(cache.visible_tiles().size() == 1);
    CHECK((cache.visible_tiles()[0] == osect::tile_key{0, 0, 0}));
}

TEST_CASE("tile_cache clamps the visible zoom level to its minimum")
{
    osect::tile_cache<resource> cache(2, 3, 256, 2);
    fake_loader loader;

    cache.update(-osect::HALF_CIRCUMFERENCE, -osect::HALF_CIRCUMFERENCE, 0.0, 0.0, 256, loader);

    REQUIRE_FALSE(cache.visible_tiles().empty());
    for(const auto& key : cache.visible_tiles())
    {
        CHECK(key.z == 2);
    }
}

TEST_CASE("tile_cache requests no tiles below its minimum zoom")
{
    osect::tile_cache<resource> cache(2, 3, 256, 2);
    fake_loader loader;

    cache.update(-osect::HALF_CIRCUMFERENCE, -osect::HALF_CIRCUMFERENCE, 0.0, 0.0, 256, loader);

    REQUIRE_FALSE(loader.requested.empty());
    for(const auto& key : loader.requested)
    {
        CHECK(key.z >= 2);
    }
}

TEST_CASE("tile_cache searches ancestors only down to its minimum zoom")
{
    osect::tile_cache<resource> cache(1, 3, 256, 2);
    cache.put({0, 0, 0}, std::make_shared<resource>());

    osect::tile_cache<resource>::fallback fallback{};
    CHECK_FALSE(cache.find_ancestor({2, 0, 0}, fallback));
}

TEST_CASE("tile_cache::fallbacks covers each uncached visible tile with its nearest cached ancestor")
{
    osect::tile_cache<resource> cache(0, 3, 256, 4);
    // Visible at zoom 1: {1, 0, 1} and {1, 1, 1}.
    fake_loader loader;
    cache.update(-osect::HALF_CIRCUMFERENCE, -osect::HALF_CIRCUMFERENCE, 0.0, 0.0, 256, loader);
    auto root = std::make_shared<resource>();
    cache.put({0, 0, 0}, root);
    cache.put({1, 1, 1}, std::make_shared<resource>());

    const auto fallbacks = cache.fallbacks();

    REQUIRE(fallbacks.size() == 1);
    CHECK((fallbacks[0].key == osect::tile_key{1, 0, 1}));
    CHECK(fallbacks[0].resource == root);
    CHECK(fallbacks[0].u0 == 0.0F);
    CHECK(fallbacks[0].v0 == 0.5F);
    CHECK(fallbacks[0].u1 == 0.5F);
    CHECK(fallbacks[0].v1 == 1.0F);
}

TEST_CASE("tile_cache::request_tile skips a cached tile")
{
    osect::tile_cache<resource> cache(0, 3, 256, 4);
    cache.put({2, 1, 1}, std::make_shared<resource>());
    fake_loader loader;

    cache.request_tile({2, 1, 1}, loader);

    CHECK(loader.requested.empty());
}

TEST_CASE("tile_cache::request_tile requests a tile that has not failed")
{
    osect::tile_cache<resource> cache(0, 3, 256, 4);
    fake_loader loader;

    cache.request_tile({2, 1, 1}, loader);

    CHECK(loader.requested == std::vector<osect::tile_key>{{2, 1, 1}});
}

TEST_CASE("tile_cache::request_tile walks up past failed tiles")
{
    osect::tile_cache<resource> cache(0, 3, 256, 4);
    fake_loader loader;
    loader.failed_keys = {{3, -1, 5}, {2, 3, 2}};

    cache.request_tile({3, -1, 5}, loader);

    CHECK(loader.requested == std::vector<osect::tile_key>{{1, 1, 1}});
}

TEST_CASE("tile_cache::request_tile stops at the minimum zoom")
{
    osect::tile_cache<resource> cache(2, 3, 256, 4);
    fake_loader loader;
    loader.failed_keys = {{3, 2, 2}, {2, 1, 1}};

    cache.request_tile({3, 2, 2}, loader);

    CHECK(loader.requested.empty());
}
