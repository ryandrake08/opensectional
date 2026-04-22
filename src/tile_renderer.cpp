#include "tile_renderer.hpp"
#include "lru_set.hpp"
#include "map_view.hpp"
#include "render_context.hpp"
#include "tile_key.hpp"
#include "tile_loader.hpp"
#include <algorithm>
#include <cmath>
#include <memory>
#include <sdl/buffer.hpp>
#include <sdl/copy_pass.hpp>
#include <sdl/device.hpp>
#include <sdl/render_pass.hpp>
#include <sdl/surface.hpp>
#include <sdl/texture.hpp>
#include <sdl/types.hpp>
#include <string>
#include <unordered_map>
#include <vector>

namespace nasrbrowse
{
    struct tile_gpu
    {
        tile_key key;
        std::unique_ptr<sdl::buffer> vertex_buffer;
        std::unique_ptr<sdl::texture> tex;

        bool operator==(const tile_gpu& other) const
        {
            return key == other.key;
        }
    };
} // namespace nasrbrowse

namespace std
{
    template<>
    struct hash<std::shared_ptr<nasrbrowse::tile_gpu>>
    {
        size_t operator()(const std::shared_ptr<nasrbrowse::tile_gpu>& p) const
        {
            return std::hash<nasrbrowse::tile_key>()(p->key);
        }
    };

    template<>
    struct equal_to<std::shared_ptr<nasrbrowse::tile_gpu>>
    {
        bool operator()(const std::shared_ptr<nasrbrowse::tile_gpu>& a,
                        const std::shared_ptr<nasrbrowse::tile_gpu>& b) const
        {
            return a->key == b->key;
        }
    };
} // namespace std

namespace nasrbrowse
{
    // Generate 6 vertices for a tile quad in Web Mercator meters.
    // UV coordinates specify the sub-region of the texture to sample
    // (normally 0-1 for the full tile, smaller range when using a
    // parent tile as a fallback).
    static void get_tile_vertices(const tile_key& key,
                                  float u0, float v0, float u1, float v1,
                                  sdl::vertex_t2f_c4ub_v3f* verts)
    {
        double mx_min, my_min, mx_max, my_max;
        tile_bounds_meters(key.x, key.y, key.z, mx_min, my_min, mx_max, my_max);

        auto x0 = static_cast<float>(mx_min);
        auto x1 = static_cast<float>(mx_max);
        auto y0 = static_cast<float>(my_min);
        auto y1 = static_cast<float>(my_max);

        uint8_t r = 255;
        uint8_t g = 255;
        uint8_t b = 255;
        uint8_t a = 255;

        // Two triangles forming a quad
        verts[0] = { u0, v1, r, g, b, a, x0, y0, 0.0F };
        verts[1] = { u1, v1, r, g, b, a, x1, y0, 0.0F };
        verts[2] = { u0, v0, r, g, b, a, x0, y1, 0.0F };
        verts[3] = { u0, v0, r, g, b, a, x0, y1, 0.0F };
        verts[4] = { u1, v1, r, g, b, a, x1, y0, 0.0F };
        verts[5] = { u1, v0, r, g, b, a, x1, y1, 0.0F };
    }

    // Compute the parent tile at a lower zoom and the UV sub-rect within it
    // that corresponds to the display tile.
    static tile_key ancestor_uv(const tile_key& display_tile, int ancestor_zoom,
                                float& u0, float& v0, float& u1, float& v1)
    {
        auto dz = display_tile.z - ancestor_zoom;
        auto scale = 1 << dz;

        // Wrap x into valid range for the display zoom
        auto n_display = 1 << display_tile.z;
        auto wx = ((display_tile.x % n_display) + n_display) % n_display;

        auto ancestor = tile_key{};
        ancestor.z = ancestor_zoom;
        ancestor.x = wx >> dz;
        ancestor.y = display_tile.y >> dz;

        auto inv_scale = 1.0F / static_cast<float>(scale);
        u0 = static_cast<float>(wx % scale) * inv_scale;
        v0 = static_cast<float>(display_tile.y % scale) * inv_scale;
        u1 = u0 + inv_scale;
        v1 = v0 + inv_scale;

        return ancestor;
    }

    struct tile_renderer::impl
    {
        sdl::device& dev;
        std::string tile_path;
        int max_zoom;

        // Current visible tile set
        std::vector<tile_key> visible_tiles;
        int current_zoom;

        // Cached tile range to avoid redundant cancel+re-request
        int cached_zoom, cached_tx_min, cached_tx_max, cached_ty_min, cached_ty_max;
        bool has_cached_range;

        // Cache: tile_key -> weak_ptr to GPU resources
        std::unordered_map<tile_key, std::weak_ptr<tile_gpu>> tile_map;

        // LRU cache owns the shared_ptrs, eviction frees GPU resources
        lru_set<std::shared_ptr<tile_gpu>> cache;

        // Background tile loader
        tile_loader loader;

        // Results drained from loader, staged for copy()
        std::vector<tile_load_result> pending_results;

        // Fallback quads for visible tiles rendered via an ancestor texture
        struct fallback_quad
        {
            std::unique_ptr<sdl::buffer> vertex_buffer;
            std::shared_ptr<tile_gpu> ancestor_gpu;
        };
        std::vector<fallback_quad> fallback_quads;
        bool fallback_dirty;

        impl(sdl::device& dev, const std::string& tile_path)
            : dev(dev)
            , tile_path(tile_path)
            , max_zoom(15)
            , current_zoom(0)
            , cached_zoom(-1)
            , cached_tx_min(0)
            , cached_tx_max(0)
            , cached_ty_min(0)
            , cached_ty_max(0)
            , has_cached_range(false)
            , cache(1024)
            , fallback_dirty(false)
        {
        }

        std::string tile_file_path(const tile_key& key) const
        {
            // Wrap x into [0, n-1] for file path (tiles repeat horizontally)
            auto n = 1 << key.z;
            auto wx = ((key.x % n) + n) % n;
            return tile_path + "/" + std::to_string(key.z) + "/" +
                   std::to_string(wx) + "/" + std::to_string(key.y) + ".png";
        }

        // Request a tile for loading. If it previously failed, walk up
        // the zoom tree and request the nearest untried ancestor.
        void request_tile(const tile_key& key)
        {
            auto it = tile_map.find(key);
            if(it != tile_map.end() && !it->second.expired())
            {
                return;
            }

            if(!loader.is_failed(key))
            {
                loader.request(key, tile_file_path(key));
                return;
            }

            // This tile has no data — request its parent
            if(key.z > 0)
            {
                auto n = 1 << key.z;
                auto wx = ((key.x % n) + n) % n;
                request_tile({ key.z - 1, wx / 2, key.y / 2 });
            }
        }

        // Find the best loaded ancestor for a tile. Returns true if found,
        // with the ancestor's GPU resources and UV sub-rect.
        bool find_ancestor(const tile_key& key, std::shared_ptr<tile_gpu>& gpu,
                           float& u0, float& v0, float& u1, float& v1)
        {
            for(int az = key.z - 1; az >= 0; az--)
            {
                auto ancestor = ancestor_uv(key, az, u0, v0, u1, v1);
                auto it = tile_map.find(ancestor);
                if(it != tile_map.end())
                {
                    gpu = it->second.lock();
                    if(gpu)
                    {
                        return true;
                    }
                }
            }
            return false;
        }
    };

    tile_renderer::tile_renderer(sdl::device& dev, const char* tile_path)
        : pimpl(std::make_unique<impl>(dev, tile_path))
    {
    }

    tile_renderer::~tile_renderer() = default;

    void tile_renderer::update(double vx_min, double vy_min,
                               double vx_max, double vy_max,
                               double, int viewport_height,
                               double)
    {
        // Compute ideal zoom level
        auto meters_per_pixel = (vy_max - vy_min) / viewport_height;
        auto world_size = 2.0 * HALF_CIRCUMFERENCE;
        auto ideal_zoom = std::log2(world_size / (256.0 * meters_per_pixel));
        auto zoom = std::max(0, std::min(pimpl->max_zoom, static_cast<int>(std::round(ideal_zoom))));
        pimpl->current_zoom = zoom;

        // Compute visible tile range
        auto n = 1 << zoom;
        auto tile_size = world_size / n;
        auto tx_min = static_cast<int>(std::floor((vx_min + HALF_CIRCUMFERENCE) / tile_size));
        auto tx_max = static_cast<int>(std::floor((vx_max + HALF_CIRCUMFERENCE) / tile_size));
        auto ty_min = static_cast<int>(std::floor((HALF_CIRCUMFERENCE - vy_max) / tile_size));
        auto ty_max = static_cast<int>(std::floor((HALF_CIRCUMFERENCE - vy_min) / tile_size));

        // tx is unbounded (wraps around the antimeridian); ty clamps to valid range
        ty_min = std::max(0, ty_min);
        ty_max = std::min(n - 1, ty_max);

        // Always rebuild visible_tiles (render needs it)
        pimpl->visible_tiles.clear();
        for(int ty = ty_min; ty <= ty_max; ty++)
        {
            for(int tx = tx_min; tx <= tx_max; tx++)
            {
                pimpl->visible_tiles.push_back({ zoom, tx, ty });
            }
        }

        // Skip cancel+re-request if tile range hasn't changed
        if(pimpl->has_cached_range &&
           zoom == pimpl->cached_zoom &&
           tx_min == pimpl->cached_tx_min && tx_max == pimpl->cached_tx_max &&
           ty_min == pimpl->cached_ty_min && ty_max == pimpl->cached_ty_max)
        {
            return;
        }

        pimpl->cached_zoom = zoom;
        pimpl->cached_tx_min = tx_min;
        pimpl->cached_tx_max = tx_max;
        pimpl->cached_ty_min = ty_min;
        pimpl->cached_ty_max = ty_max;
        pimpl->has_cached_range = true;
        pimpl->fallback_dirty = true;

        pimpl->loader.cancel();

        // Request visible tiles (walks up to ancestors for failed tiles)
        for(const auto& key : pimpl->visible_tiles)
        {
            pimpl->request_tile(key);
        }

        // Prefetch: 1-tile border around visible area at current zoom
        auto border_tx_min = tx_min - 1;
        auto border_tx_max = tx_max + 1;
        auto border_ty_min = std::max(0, ty_min - 1);
        auto border_ty_max = std::min(n - 1, ty_max + 1);

        for(int ty = border_ty_min; ty <= border_ty_max; ty++)
        {
            for(int tx = border_tx_min; tx <= border_tx_max; tx++)
            {
                pimpl->request_tile({ zoom, tx, ty });
            }
        }

        // Prefetch: zoom +1 tiles overlapping viewport
        if(zoom + 1 <= pimpl->max_zoom)
        {
            auto nz = 1 << (zoom + 1);
            auto tsz = world_size / nz;
            auto ztx_min = static_cast<int>(std::floor((vx_min + HALF_CIRCUMFERENCE) / tsz));
            auto ztx_max = static_cast<int>(std::floor((vx_max + HALF_CIRCUMFERENCE) / tsz));
            auto zty_min = std::max(0, static_cast<int>(std::floor((HALF_CIRCUMFERENCE - vy_max) / tsz)));
            auto zty_max = std::min(nz - 1, static_cast<int>(std::floor((HALF_CIRCUMFERENCE - vy_min) / tsz)));

            for(int ty = zty_min; ty <= zty_max; ty++)
            {
                for(int tx = ztx_min; tx <= ztx_max; tx++)
                {
                    pimpl->request_tile({ zoom + 1, tx, ty });
                }
            }
        }

        // Prefetch: zoom -1 tiles overlapping viewport
        if(zoom - 1 >= 0)
        {
            auto nz = 1 << (zoom - 1);
            auto tsz = world_size / nz;
            auto ztx_min = static_cast<int>(std::floor((vx_min + HALF_CIRCUMFERENCE) / tsz));
            auto ztx_max = static_cast<int>(std::floor((vx_max + HALF_CIRCUMFERENCE) / tsz));
            auto zty_min = std::max(0, static_cast<int>(std::floor((HALF_CIRCUMFERENCE - vy_max) / tsz)));
            auto zty_max = std::min(nz - 1, static_cast<int>(std::floor((HALF_CIRCUMFERENCE - vy_min) / tsz)));

            for(int ty = zty_min; ty <= zty_max; ty++)
            {
                for(int tx = ztx_min; tx <= ztx_max; tx++)
                {
                    pimpl->request_tile({ zoom - 1, tx, ty });
                }
            }
        }
    }

    void tile_renderer::drain()
    {
        auto results = pimpl->loader.drain_results();
        if(!results.empty())
        {
            pimpl->fallback_dirty = true;
        }
        for(auto& result : results)
        {
            pimpl->pending_results.push_back(std::move(result));
        }
    }

    bool tile_renderer::needs_upload() const
    {
        return !pimpl->pending_results.empty() || pimpl->fallback_dirty;
    }

    void tile_renderer::copy(sdl::copy_pass& pass)
    {
        // Upload newly loaded tiles
        for(auto& result : pimpl->pending_results)
        {
            auto vertices = std::vector<sdl::vertex_t2f_c4ub_v3f>(6);
            get_tile_vertices(result.key, 0.0F, 0.0F, 1.0F, 1.0F, vertices.data());

            auto gpu = std::make_shared<tile_gpu>();
            gpu->key = result.key;

            auto vbuf = pass.create_and_upload_buffer(pimpl->dev, sdl::buffer_usage::vertex, vertices);
            gpu->vertex_buffer = std::make_unique<sdl::buffer>(std::move(vbuf));

            auto tex = pass.create_and_upload_texture(pimpl->dev, *result.surf);
            gpu->tex = std::make_unique<sdl::texture>(std::move(tex));

            pimpl->cache.put(gpu);
            pimpl->tile_map[result.key] = gpu;
        }
        pimpl->pending_results.clear();

        // Build fallback quads for visible tiles that don't have a direct
        // match — find the best loaded ancestor and render with UV sub-rect
        pimpl->fallback_quads.clear();
        pimpl->fallback_dirty = false;

        for(const auto& key : pimpl->visible_tiles)
        {
            // Skip tiles that have a direct match (rendered normally)
            auto it = pimpl->tile_map.find(key);
            if(it != pimpl->tile_map.end() && !it->second.expired())
            {
                continue;
            }

            auto u0 = 0.0F;
            auto v0 = 0.0F;
            auto u1 = 0.0F;
            auto v1 = 0.0F;
            auto ancestor_gpu = std::shared_ptr<tile_gpu>();
            if(!pimpl->find_ancestor(key, ancestor_gpu, u0, v0, u1, v1))
            {
                continue;
            }

            pimpl->cache.get(ancestor_gpu);

            auto verts = std::vector<sdl::vertex_t2f_c4ub_v3f>(6);
            get_tile_vertices(key, u0, v0, u1, v1, verts.data());

            auto vbuf = pass.create_and_upload_buffer(pimpl->dev, sdl::buffer_usage::vertex, verts);

            impl::fallback_quad quad;
            quad.vertex_buffer = std::make_unique<sdl::buffer>(std::move(vbuf));
            quad.ancestor_gpu = ancestor_gpu;
            pimpl->fallback_quads.push_back(std::move(quad));
        }
    }

    void tile_renderer::render(sdl::render_pass& pass, const render_context& ctx, const glm::mat4& view_matrix) const
    {
        if(ctx.current_pass != render_pass_id::textured_trianglelist_0)
        {
            return;
        }

        auto uniforms = sdl::uniform_buffer{};
        uniforms.projection_matrix = ctx.projection_matrix;
        uniforms.view_matrix = view_matrix;

        // Render direct-match tiles
        for(const auto& key : pimpl->visible_tiles)
        {
            auto it = pimpl->tile_map.find(key);
            if(it == pimpl->tile_map.end())
            {
                continue;
            }
            auto gpu = it->second.lock();
            if(!gpu)
            {
                continue;
            }

            pimpl->cache.get(gpu);

            pass.push_vertex_uniforms(0, &uniforms, sizeof(uniforms));
            pass.push_fragment_uniforms(0, &uniforms, sizeof(uniforms));
            pass.bind_vertex_buffer(*gpu->vertex_buffer);
            pass.bind_fragment_texture_sampler(0, *gpu->tex, ctx.sampler);
            pass.draw(6);
        }

        // Render fallback quads (ancestor tiles with UV sub-rects)
        for(const auto& quad : pimpl->fallback_quads)
        {
            pass.push_vertex_uniforms(0, &uniforms, sizeof(uniforms));
            pass.push_fragment_uniforms(0, &uniforms, sizeof(uniforms));
            pass.bind_vertex_buffer(*quad.vertex_buffer);
            pass.bind_fragment_texture_sampler(0, *quad.ancestor_gpu->tex, ctx.sampler);
            pass.draw(6);
        }
    }

} // namespace nasrbrowse
