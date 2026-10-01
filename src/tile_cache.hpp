#pragma once

#include "lru_map.hpp"
#include "map_view.hpp"
#include "tile_key.hpp"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iterator>
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
            : min_zoom(min_zoom), max_zoom(max_zoom), tile_pixels(tile_pixels), retained(capacity)
        {
        }

        // Rebuilds the visible set for the view and requests tiles from
        // loader. When the visible range changes, drops loader's queued
        // loads and requests the new range: the visible tiles, a one-tile
        // ring around them, and the adjacent zoom levels. Otherwise
        // re-requests the visible tiles, so a tile that failed to load is
        // replaced by its nearest unfailed ancestor as soon as the failure
        // is known. loader provides failed(key), request(key), and cancel().
        // Returns true when the range changed.
        template <typename loader_t>
        bool update(double vx_min, double vy_min, double vx_max, double vy_max, int viewport_height, loader_t& loader)
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
                request_visible(loader);
                return false;
            }

            cached_zoom = current_zoom;
            cached_tx_min = tx_min;
            cached_tx_max = tx_max;
            cached_ty_min = ty_min;
            cached_ty_max = ty_max;
            has_cached_range = true;

            loader.cancel();
            request_visible(loader);
            request_range(current_zoom, tx_min - 1, tx_max + 1, ty_min - 1, ty_max + 1, loader);
            request_range(current_zoom + 1, vx_min, vy_min, vx_max, vy_max, loader);
            request_range(current_zoom - 1, vx_min, vy_min, vx_max, vy_max, loader);
            return true;
        }

        const std::vector<tile_key>& visible_tiles() const
        {
            return visible;
        }

        void put(const tile_key& key, std::shared_ptr<resource_t> resource)
        {
            resources[key] = resource;
            retained.put(key, std::move(resource));

            // An entry outlives its tile once nothing holds the tile; sweep
            // the expired entries when they outnumber the retained set.
            if(resources.size() > 2 * retained.capacity())
            {
                for(auto it = resources.begin(); it != resources.end();)
                {
                    it = it->second.expired() ? resources.erase(it) : std::next(it);
                }
            }
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
                retained.put(key, resource);
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

        // Stored tiles the cache still tracks: every tile it can find, plus
        // freed tiles not yet swept. Bounded by twice the capacity plus the
        // evicted tiles held elsewhere.
        std::size_t tracked_count() const
        {
            return resources.size();
        }

        // For each visible tile that isn't cached, the nearest cached
        // ancestor to draw in its place. Tiles with no cached ancestor are
        // left out.
        std::vector<fallback> fallbacks()
        {
            std::vector<fallback> result;
            for(const auto& key : visible)
            {
                fallback entry{};
                if(!find(key) && find_ancestor(key, entry))
                {
                    result.push_back(std::move(entry));
                }
            }
            return result;
        }

        // Requests key from loader unless it's cached. A key that already
        // failed to load is replaced by its nearest ancestor that hasn't,
        // stopping at min_zoom. loader provides failed(key) and request(key).
        template <typename loader_t>
        void request_tile(tile_key key, loader_t& loader)
        {
            while(!find(key))
            {
                if(!loader.failed(key))
                {
                    loader.request(key);
                    return;
                }
                if(key.z <= min_zoom)
                {
                    return;
                }
                key = key.parent();
            }
        }

    private:
        int min_zoom;
        int max_zoom;
        int tile_pixels;
        int current_zoom = 0;
        std::vector<tile_key> visible;
        int cached_zoom = -1;
        int cached_tx_min = 0;
        int cached_tx_max = 0;
        int cached_ty_min = 0;
        int cached_ty_max = 0;
        bool has_cached_range = false;
        // Every resource ever stored, found for as long as something holds
        // it; retained keeps the most recently used alive.
        std::unordered_map<tile_key, std::weak_ptr<resource_t>> resources;
        lru_map<tile_key, std::shared_ptr<resource_t>> retained;

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

        template <typename loader_t>
        void request_visible(loader_t& loader)
        {
            for(const auto& key : visible)
            {
                request_tile(key, loader);
            }
        }

        template <typename loader_t>
        void request_range(int zoom, int tx_min, int tx_max, int ty_min, int ty_max, loader_t& loader)
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
                    request_tile({zoom, tx, ty}, loader);
                }
            }
        }

        template <typename loader_t>
        void request_range(int zoom, double vx_min, double vy_min, double vx_max, double vy_max, loader_t& loader)
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
            request_range(zoom, tx_min, tx_max, ty_min, ty_max, loader);
        }
    };
} // namespace osect
