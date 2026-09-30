#pragma once

#include "map_view.hpp"
#include "tile_key.hpp"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <list>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

namespace osect
{
    // Selects, retains, and resolves tiles independently of a renderer's
    // loader and GPU resource type.
    template <typename resource_t>
    class tile_cache
    {
    public:
        struct fallback
        {
            tile_key key;
            float u0;
            float v0;
            float u1;
            float v1;
            std::shared_ptr<resource_t> resource;
        };

        // Tiles exist at zoom levels min_zoom..max_zoom, each tile_pixels
        // on a side.
        tile_cache(int min_zoom, int max_zoom, int tile_pixels, std::size_t capacity)
            : min_zoom(min_zoom), max_zoom(max_zoom), tile_pixels(tile_pixels), capacity(capacity)
        {
        }

        // Rebuild the visible set and request its tiles plus nearby tiles.
        // Returns true when the requested range changed.
        bool update(double vx_min, double vy_min, double vx_max, double vy_max, int viewport_height,
                    const std::function<void()>& reset_requests, const std::function<void(const tile_key&)>& request)
        {
            auto meters_per_pixel = (vy_max - vy_min) / viewport_height;
            auto world_size = 2.0 * HALF_CIRCUMFERENCE;
            auto ideal_zoom = std::log2(world_size / (tile_pixels * meters_per_pixel));
            current_zoom = std::max(min_zoom, std::min(max_zoom, static_cast<int>(std::round(ideal_zoom))));

            auto n = 1 << current_zoom;
            auto tile_size = world_size / n;
            auto tx_min = static_cast<int>(std::floor((vx_min + HALF_CIRCUMFERENCE) / tile_size));
            auto tx_max = static_cast<int>(std::floor((vx_max + HALF_CIRCUMFERENCE) / tile_size));
            auto ty_min = std::max(0, static_cast<int>(std::floor((HALF_CIRCUMFERENCE - vy_max) / tile_size)));
            auto ty_max = std::min(n - 1, static_cast<int>(std::floor((HALF_CIRCUMFERENCE - vy_min) / tile_size)));

            visible.clear();
            for(int ty = ty_min; ty <= ty_max; ty++)
            {
                for(int tx = tx_min; tx <= tx_max; tx++)
                {
                    visible.push_back({current_zoom, tx, ty});
                }
            }

            if(has_cached_range && current_zoom == cached_zoom && tx_min == cached_tx_min && tx_max == cached_tx_max &&
               ty_min == cached_ty_min && ty_max == cached_ty_max)
            {
                return false;
            }

            cached_zoom = current_zoom;
            cached_tx_min = tx_min;
            cached_tx_max = tx_max;
            cached_ty_min = ty_min;
            cached_ty_max = ty_max;
            has_cached_range = true;

            reset_requests();

            for(const auto& key : visible)
            {
                request(key);
            }

            request_range(current_zoom, tx_min - 1, tx_max + 1, ty_min - 1, ty_max + 1, request);
            request_range(current_zoom + 1, vx_min, vy_min, vx_max, vy_max, request);
            request_range(current_zoom - 1, vx_min, vy_min, vx_max, vy_max, request);
            return true;
        }

        const std::vector<tile_key>& visible_tiles() const
        {
            return visible;
        }

        void put(const tile_key& key, std::shared_ptr<resource_t> resource)
        {
            resources[key] = resource;
            touch(key, std::move(resource));
        }

        std::shared_ptr<resource_t> find(const tile_key& key)
        {
            auto it = resources.find(key);
            if(it == resources.end())
            {
                return nullptr;
            }

            auto resource = it->second.lock();
            if(resource)
            {
                touch(key, resource);
            }
            return resource;
        }

        bool find_ancestor(const tile_key& key, fallback& result)
        {
            for(int ancestor_zoom = key.z - 1; ancestor_zoom >= min_zoom; ancestor_zoom--)
            {
                auto ancestor = ancestor_uv(key, ancestor_zoom, result.u0, result.v0, result.u1, result.v1);
                auto resource = find(ancestor);
                if(resource)
                {
                    result.key = key;
                    result.resource = std::move(resource);
                    return true;
                }
            }
            return false;
        }

    private:
        int min_zoom;
        int max_zoom;
        int tile_pixels;
        std::size_t capacity;
        int current_zoom = 0;
        std::vector<tile_key> visible;
        int cached_zoom = -1;
        int cached_tx_min = 0;
        int cached_tx_max = 0;
        int cached_ty_min = 0;
        int cached_ty_max = 0;
        bool has_cached_range = false;
        std::unordered_map<tile_key, std::weak_ptr<resource_t>> resources;
        std::list<std::pair<tile_key, std::shared_ptr<resource_t>>> lru;
        std::unordered_map<tile_key, typename std::list<std::pair<tile_key, std::shared_ptr<resource_t>>>::iterator>
            lru_index;

        static tile_key ancestor_uv(const tile_key& display_tile, int ancestor_zoom, float& u0, float& v0, float& u1,
                                    float& v1)
        {
            auto zoom_delta = display_tile.z - ancestor_zoom;
            auto scale = 1 << zoom_delta;
            auto wrapped_x = display_tile.wrapped().x;

            u0 = static_cast<float>((wrapped_x % scale) / static_cast<double>(scale));
            v0 = static_cast<float>((display_tile.y % scale) / static_cast<double>(scale));
            u1 = static_cast<float>(u0 + 1.0 / scale);
            v1 = static_cast<float>(v0 + 1.0 / scale);
            return {ancestor_zoom, wrapped_x / scale, display_tile.y / scale};
        }

        void touch(const tile_key& key, std::shared_ptr<resource_t> resource)
        {
            auto it = lru_index.find(key);
            if(it != lru_index.end())
            {
                lru.erase(it->second);
            }
            lru.push_front({key, std::move(resource)});
            lru_index[key] = lru.begin();

            if(lru_index.size() > capacity)
            {
                auto last = std::prev(lru.end());
                lru_index.erase(last->first);
                lru.pop_back();
            }
        }

        void request_range(int zoom, int tx_min, int tx_max, int ty_min, int ty_max,
                           const std::function<void(const tile_key&)>& request) const
        {
            if(zoom < min_zoom || zoom > max_zoom)
            {
                return;
            }

            auto n = 1 << zoom;
            for(int ty = std::max(0, ty_min); ty <= std::min(n - 1, ty_max); ty++)
            {
                for(int tx = tx_min; tx <= tx_max; tx++)
                {
                    request({zoom, tx, ty});
                }
            }
        }

        void request_range(int zoom, double vx_min, double vy_min, double vx_max, double vy_max,
                           const std::function<void(const tile_key&)>& request) const
        {
            if(zoom < min_zoom || zoom > max_zoom)
            {
                return;
            }

            auto n = 1 << zoom;
            auto tile_size = 2.0 * HALF_CIRCUMFERENCE / n;
            auto tx_min = static_cast<int>(std::floor((vx_min + HALF_CIRCUMFERENCE) / tile_size));
            auto tx_max = static_cast<int>(std::floor((vx_max + HALF_CIRCUMFERENCE) / tile_size));
            auto ty_min = static_cast<int>(std::floor((HALF_CIRCUMFERENCE - vy_max) / tile_size));
            auto ty_max = static_cast<int>(std::floor((HALF_CIRCUMFERENCE - vy_min) / tile_size));
            request_range(zoom, tx_min, tx_max, ty_min, ty_max, request);
        }
    };
} // namespace osect
