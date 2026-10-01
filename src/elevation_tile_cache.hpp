#pragma once

#include "lru_map.hpp"
#include "tile_key.hpp"
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>

namespace osect
{
    class elevation_tile;

    // Thread-safe cache of the most recently used elevation tiles. The
    // caller supplies how a tile is loaded; a key whose load returned null
    // (no tile on disk) is cached as null.
    class elevation_tile_cache
    {
        std::mutex mutex_;
        lru_map<tile_key, std::shared_ptr<const elevation_tile>> tiles_;

    public:
        explicit elevation_tile_cache(std::size_t capacity);

        // Returns the cached tile for key, or calls load (without holding
        // the cache lock) and caches its result, evicting the least
        // recently used tile beyond capacity.
        std::shared_ptr<const elevation_tile> get(const tile_key& key,
                                                  const std::function<std::shared_ptr<const elevation_tile>()>& load);
    };
} // namespace osect
