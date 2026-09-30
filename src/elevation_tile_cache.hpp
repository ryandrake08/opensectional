#pragma once

#include "tile_key.hpp"
#include <cstddef>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace osect
{
    class elevation_tile;

    // Thread-safe cache of the most recently used elevation tiles. The
    // caller supplies how a tile is loaded; a key whose load returned null
    // (no tile on disk) is cached as null.
    class elevation_tile_cache
    {
        struct entry
        {
            std::shared_ptr<const elevation_tile> tile;
            std::list<tile_key>::iterator recency;
        };

        std::size_t capacity_;
        std::mutex mutex_;
        std::unordered_map<tile_key, entry> entries_;
        std::list<tile_key> recency_; // most recently used first

        // Promotes an entry to most recently used. Caller holds mutex_.
        void touch(std::unordered_map<tile_key, entry>::iterator it);

    public:
        explicit elevation_tile_cache(std::size_t capacity);

        // Returns the cached tile for key, or calls load (without holding
        // the cache lock) and caches its result, evicting the least
        // recently used tile beyond capacity.
        std::shared_ptr<const elevation_tile> get(const tile_key& key,
                                                  const std::function<std::shared_ptr<const elevation_tile>()>& load);
    };
} // namespace osect
