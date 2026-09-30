#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "elevation_tile.hpp"
#include "elevation_tile_cache.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <stdexcept>

namespace
{
    std::shared_ptr<const osect::elevation_tile> make_tile()
    {
        const std::array<uint8_t, 4> pixel{128, 0, 0, 255};
        return std::make_shared<osect::elevation_tile>(osect::elevation_tile::from_rgba(1, 1, pixel.data(), 4));
    }

    // Loader that hands out a fresh tile and counts its calls.
    struct counting_loader
    {
        int calls = 0;

        std::shared_ptr<const osect::elevation_tile> operator()()
        {
            calls++;
            return make_tile();
        }
    };
}

TEST_CASE("elevation_tile_cache loads a key once and then returns the cached tile")
{
    osect::elevation_tile_cache cache(2);
    counting_loader load;

    const auto first = cache.get({3, 1, 2}, std::ref(load));
    const auto second = cache.get({3, 1, 2}, std::ref(load));

    CHECK(load.calls == 1);
    CHECK(first == second);
}

TEST_CASE("elevation_tile_cache caches an empty load result")
{
    osect::elevation_tile_cache cache(2);
    int calls = 0;
    const auto load_nothing = [&calls]() -> std::shared_ptr<const osect::elevation_tile>
    {
        calls++;
        return nullptr;
    };

    CHECK_FALSE(cache.get({3, 1, 2}, load_nothing));
    CHECK_FALSE(cache.get({3, 1, 2}, load_nothing));
    CHECK(calls == 1);
}

TEST_CASE("elevation_tile_cache evicts the least recently used tile at capacity")
{
    osect::elevation_tile_cache cache(2);
    counting_loader load;
    const osect::tile_key a{3, 0, 0};
    const osect::tile_key b{3, 1, 0};
    const osect::tile_key c{3, 2, 0};

    cache.get(a, std::ref(load));
    cache.get(b, std::ref(load));
    cache.get(a, std::ref(load)); // a is now more recent than b
    cache.get(c, std::ref(load)); // evicts b
    REQUIRE(load.calls == 3);

    cache.get(a, std::ref(load));
    CHECK(load.calls == 3);
    cache.get(b, std::ref(load));
    CHECK(load.calls == 4);
}

TEST_CASE("elevation_tile_cache calls the loader without holding its lock")
{
    osect::elevation_tile_cache cache(2);
    counting_loader inner;
    const auto load_with_nested_lookup = [&cache, &inner]
    {
        // Deadlocks if get() holds the cache lock while loading.
        cache.get({3, 5, 5}, std::ref(inner));
        return make_tile();
    };

    CHECK(cache.get({3, 1, 1}, load_with_nested_lookup));
    CHECK(inner.calls == 1);
}

TEST_CASE("elevation_tile_cache rejects a zero capacity")
{
    CHECK_THROWS_AS(osect::elevation_tile_cache(0), std::invalid_argument);
}
