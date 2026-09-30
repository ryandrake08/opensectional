#include "elevation_tile_cache.hpp"
#include "elevation_tile.hpp"
#include <stdexcept>

namespace osect
{
    elevation_tile_cache::elevation_tile_cache(std::size_t capacity) : capacity_(capacity)
    {
        if(capacity_ == 0)
        {
            throw std::invalid_argument("elevation tile cache capacity must be positive");
        }
    }

    std::shared_ptr<const elevation_tile> elevation_tile_cache::get(
        const tile_key& key, const std::function<std::shared_ptr<const elevation_tile>()>& load)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        const auto cached = entries_.find(key);
        if(cached != entries_.end())
        {
            touch(cached);
            return cached->second.tile;
        }
        lock.unlock();

        std::shared_ptr<const elevation_tile> tile = load();

        lock.lock();
        // Another thread may have loaded the same key while unlocked; keep
        // the entry already cached.
        const auto inserted = entries_.emplace(key, entry{tile, recency_.end()});
        if(!inserted.second)
        {
            touch(inserted.first);
            return inserted.first->second.tile;
        }
        recency_.push_front(key);
        inserted.first->second.recency = recency_.begin();
        if(entries_.size() > capacity_)
        {
            entries_.erase(recency_.back());
            recency_.pop_back();
        }
        return tile;
    }

    void elevation_tile_cache::touch(std::unordered_map<tile_key, entry>::iterator it)
    {
        recency_.splice(recency_.begin(), recency_, it->second.recency);
        it->second.recency = recency_.begin();
    }
} // namespace osect
