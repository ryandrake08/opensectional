#include "elevation_tile_cache.hpp"
#include "elevation_tile.hpp"
#include <stdexcept>

namespace osect
{
    elevation_tile_cache::elevation_tile_cache(std::size_t capacity) : tiles_(capacity)
    {
        if(capacity == 0)
        {
            throw std::invalid_argument("elevation tile cache capacity must be positive");
        }
    }

    std::shared_ptr<const elevation_tile> elevation_tile_cache::get(
        const tile_key& key, const std::function<std::shared_ptr<const elevation_tile>()>& load)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if(const auto* cached = tiles_.find(key))
        {
            return *cached;
        }
        lock.unlock();

        std::shared_ptr<const elevation_tile> tile = load();

        lock.lock();
        // Another thread may have loaded the same key while unlocked; keep
        // the entry already cached.
        if(const auto* cached = tiles_.find(key))
        {
            return *cached;
        }
        tiles_.put(key, tile);
        return tile;
    }
} // namespace osect
