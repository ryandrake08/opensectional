#include "tile_renderer.hpp"
#include "lru_set.hpp"
#include "map_view.hpp"
#include "render_context.hpp"
#include "tile_key.hpp"
#include "tile_loader.hpp"
#include <algorithm>
#include <cmath>
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
    // Generate 6 vertices for a tile quad in Web Mercator meters
    static void get_tile_vertices(const tile_key& key, sdl::vertex_t2f_c4ub_v3f* verts)
    {
        double mx_min, my_min, mx_max, my_max;
        tile_bounds_meters(key.x, key.y, key.z, mx_min, my_min, mx_max, my_max);

        float x0 = static_cast<float>(mx_min);
        float x1 = static_cast<float>(mx_max);
        float y0 = static_cast<float>(my_min);
        float y1 = static_cast<float>(my_max);

        uint8_t r = 255, g = 255, b = 255, a = 255;

        // Two triangles forming a quad
        verts[0] = { 0.0F, 1.0F, r, g, b, a, x0, y0, 0.0F };
        verts[1] = { 1.0F, 1.0F, r, g, b, a, x1, y0, 0.0F };
        verts[2] = { 0.0F, 0.0F, r, g, b, a, x0, y1, 0.0F };
        verts[3] = { 0.0F, 0.0F, r, g, b, a, x0, y1, 0.0F };
        verts[4] = { 1.0F, 1.0F, r, g, b, a, x1, y0, 0.0F };
        verts[5] = { 1.0F, 0.0F, r, g, b, a, x1, y1, 0.0F };
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
        {
        }

        std::string tile_file_path(const tile_key& key) const
        {
            // Wrap x into [0, n-1] for file path (tiles repeat horizontally)
            int n = 1 << key.z;
            int wx = ((key.x % n) + n) % n;
            return tile_path + "/" + std::to_string(key.z) + "/" +
                   std::to_string(wx) + "/" + std::to_string(key.y) + ".png";
        }

        void request_tile(const tile_key& key)
        {
            auto it = tile_map.find(key);
            if(it == tile_map.end() || it->second.expired())
            {
                loader.request(key, tile_file_path(key));
            }
        }
    };

    tile_renderer::tile_renderer(sdl::device& dev, const char* tile_path)
        : pimpl(new impl(dev, tile_path))
    {
    }

    tile_renderer::~tile_renderer() = default;

    void tile_renderer::update(double vx_min, double vy_min,
                               double vx_max, double vy_max,
                               double, int viewport_height,
                               double)
    {
        // Compute ideal zoom level
        double meters_per_pixel = (vy_max - vy_min) / viewport_height;
        double world_size = 2.0 * HALF_CIRCUMFERENCE;
        double ideal_zoom = std::log2(world_size / (256.0 * meters_per_pixel));
        int zoom = std::max(0, std::min(pimpl->max_zoom, static_cast<int>(std::round(ideal_zoom))));
        pimpl->current_zoom = zoom;

        // Compute visible tile range
        int n = 1 << zoom;
        double tile_size = world_size / n;
        int tx_min = static_cast<int>(std::floor((vx_min + HALF_CIRCUMFERENCE) / tile_size));
        int tx_max = static_cast<int>(std::floor((vx_max + HALF_CIRCUMFERENCE) / tile_size));
        int ty_min = static_cast<int>(std::floor((HALF_CIRCUMFERENCE - vy_max) / tile_size));
        int ty_max = static_cast<int>(std::floor((HALF_CIRCUMFERENCE - vy_min) / tile_size));

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

        pimpl->loader.cancel();

        // Request visible tiles (highest priority — enqueued first)
        for(const auto& key : pimpl->visible_tiles)
        {
            pimpl->request_tile(key);
        }

        // Prefetch: 1-tile border around visible area at current zoom
        int border_tx_min = tx_min - 1;
        int border_tx_max = tx_max + 1;
        int border_ty_min = std::max(0, ty_min - 1);
        int border_ty_max = std::min(n - 1, ty_max + 1);

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
            int nz = 1 << (zoom + 1);
            double tsz = world_size / nz;
            int ztx_min = static_cast<int>(std::floor((vx_min + HALF_CIRCUMFERENCE) / tsz));
            int ztx_max = static_cast<int>(std::floor((vx_max + HALF_CIRCUMFERENCE) / tsz));
            int zty_min = std::max(0, static_cast<int>(std::floor((HALF_CIRCUMFERENCE - vy_max) / tsz)));
            int zty_max = std::min(nz - 1, static_cast<int>(std::floor((HALF_CIRCUMFERENCE - vy_min) / tsz)));

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
            int nz = 1 << (zoom - 1);
            double tsz = world_size / nz;
            int ztx_min = static_cast<int>(std::floor((vx_min + HALF_CIRCUMFERENCE) / tsz));
            int ztx_max = static_cast<int>(std::floor((vx_max + HALF_CIRCUMFERENCE) / tsz));
            int zty_min = std::max(0, static_cast<int>(std::floor((HALF_CIRCUMFERENCE - vy_max) / tsz)));
            int zty_max = std::min(nz - 1, static_cast<int>(std::floor((HALF_CIRCUMFERENCE - vy_min) / tsz)));

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
        for(auto& result : results)
        {
            pimpl->pending_results.push_back(std::move(result));
        }
    }

    bool tile_renderer::needs_upload() const
    {
        return !pimpl->pending_results.empty();
    }

    void tile_renderer::copy(sdl::copy_pass& pass)
    {
        if(pimpl->pending_results.empty())
        {
            return;
        }

        for(auto& result : pimpl->pending_results)
        {
            std::vector<sdl::vertex_t2f_c4ub_v3f> vertices(6);
            get_tile_vertices(result.key, vertices.data());

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
    }

    void tile_renderer::render(sdl::render_pass& pass, const render_context& ctx, const glm::mat4& view_matrix) const
    {
        if(ctx.current_pass != render_pass_id::textured_trianglelist_0)
        {
            return;
        }

        sdl::uniform_buffer uniforms;
        uniforms.projection_matrix = ctx.projection_matrix;
        uniforms.view_matrix = view_matrix;

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
    }

} // namespace nasrbrowse
