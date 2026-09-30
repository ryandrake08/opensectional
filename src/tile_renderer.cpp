#include "tile_renderer.hpp"
#include "map_view.hpp"
#include "render_context.hpp"
#include "tile_cache.hpp"
#include "tile_key.hpp"
#include "tile_loader.hpp"
#include <algorithm>
#include <cmath>
#include <memory>
#include <sdl/buffer.hpp>
#include <sdl/copy_pass.hpp>
#include <sdl/device.hpp>
#include <sdl/pipeline.hpp>
#include <sdl/render_pass.hpp>
#include <sdl/sampler.hpp>
#include <sdl/surface.hpp>
#include <sdl/texture.hpp>
#include <sdl/transfer_buffer.hpp>
#include <sdl/uniform_buffer.hpp>
#include <string>
#include <vector>

namespace osect
{
    struct tile_gpu
    {
        tile_key key;
        sdl::buffer vertex_buffer;
        sdl::texture tex;
    };
} // namespace osect

namespace osect
{
    // Generate 6 vertices for a tile quad in Web Mercator meters.
    // UV coordinates specify the sub-region of the texture to sample
    // (normally 0-1 for the full tile, smaller range when using a
    // parent tile as a fallback).
    static void get_tile_vertices(const tile_key& key, float u0, float v0, float u1, float v1,
                                  sdl::vertex_t2f_c4ub_v3f* verts)
    {
        auto [mx_min, my_min, mx_max, my_max] = tile_bounds_meters(key.x, key.y, key.z);

        auto x0 = static_cast<float>(mx_min);
        auto x1 = static_cast<float>(mx_max);
        auto y0 = static_cast<float>(my_min);
        auto y1 = static_cast<float>(my_max);

        uint8_t r = 255;
        uint8_t g = 255;
        uint8_t b = 255;
        uint8_t a = 255;

        // Two triangles forming a quad
        verts[0] = {u0, v1, r, g, b, a, x0, y0, 0.0F};
        verts[1] = {u1, v1, r, g, b, a, x1, y0, 0.0F};
        verts[2] = {u0, v0, r, g, b, a, x0, y1, 0.0F};
        verts[3] = {u0, v0, r, g, b, a, x0, y1, 0.0F};
        verts[4] = {u1, v1, r, g, b, a, x1, y0, 0.0F};
        verts[5] = {u1, v0, r, g, b, a, x1, y1, 0.0F};
    }

    struct tile_renderer::impl
    {
        sdl::device& dev;
        sdl::sampler sampler;
        std::filesystem::path tile_path;
        tile_cache<tile_gpu> cache;

        // Background tile loader
        tile_loader loader;

        // Results drained from loader, staged for copy()
        std::vector<tile_load_result> pending_results;

        // Fallback quads for visible tiles rendered via an ancestor texture
        struct fallback_quad
        {
            sdl::buffer vertex_buffer;
            std::shared_ptr<tile_gpu> ancestor_gpu;
        };
        std::vector<fallback_quad> fallback_quads;
        bool fallback_dirty = false;

        impl(sdl::device& dev, std::filesystem::path tile_path)
            : dev(dev),
              sampler(dev, sdl::filter::linear, sdl::filter::linear, sdl::sampler_address_mode::clamp_to_edge),
              tile_path(std::move(tile_path)),
              cache(0, 15, 256, 1024)
        {
        }

        std::filesystem::path tile_file_path(const tile_key& key) const
        {
            // Wrap x into [0, n-1] for file path (tiles repeat horizontally)
            auto n = 1 << key.z;
            auto wx = ((key.x % n) + n) % n;
            return tile_path / std::to_string(key.z) / std::to_string(wx) / (std::to_string(key.y) + ".png");
        }

        // Request a tile for loading. If it previously failed, walk up
        // the zoom tree and request the nearest untried ancestor.
        void request_tile(const tile_key& key)
        {
            if(cache.find(key))
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
                request_tile({key.z - 1, wx / 2, key.y / 2});
            }
        }

        // Plan one copy() call: resolve fallback ancestors for the current
        // visible set and sum up every byte the transfer buffer will carry
        // (vertex buffers + tile texture pixels + fallback vertex buffers).
        std::pair<std::vector<typename tile_cache<tile_gpu>::fallback>, uint32_t> plan_uploads()
        {
            constexpr uint32_t vbuf_bytes = 6 * sizeof(sdl::vertex_t2f_c4ub_v3f);

            std::vector<typename tile_cache<tile_gpu>::fallback> fallbacks;
            fallbacks.reserve(cache.visible_tiles().size());
            for(const auto& key : cache.visible_tiles())
            {
                if(cache.find(key))
                {
                    continue;
                }
                typename tile_cache<tile_gpu>::fallback fallback{};
                if(!cache.find_ancestor(key, fallback))
                {
                    continue;
                }
                fallbacks.push_back(std::move(fallback));
            }

            uint32_t total_bytes = 0;
            for(const auto& result : pending_results)
            {
                total_bytes += vbuf_bytes + result.surf.size();
            }
            total_bytes += static_cast<uint32_t>(fallbacks.size()) * vbuf_bytes;

            return {std::move(fallbacks), total_bytes};
        }
    };

    tile_renderer::tile_renderer(sdl::device& dev, const std::filesystem::path& tile_path)
        : pimpl(std::make_unique<impl>(dev, tile_path))
    {
    }

    tile_renderer::~tile_renderer() = default;

    void tile_renderer::update(double vx_min, double vy_min, double vx_max, double vy_max, double /*half_extent_y*/,
                               int viewport_height, double /*aspect_ratio*/)
    {
        pimpl->cache.update(
            vx_min, vy_min, vx_max, vy_max, viewport_height,
            [this]
            {
                pimpl->fallback_dirty = true;
                pimpl->loader.cancel();
            },
            [this](const tile_key& key) { pimpl->request_tile(key); });
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
        pimpl->fallback_quads.clear();
        pimpl->fallback_dirty = false;

        auto [fallbacks, total_bytes] = pimpl->plan_uploads();
        if(total_bytes == 0)
        {
            return;
        }

        sdl::transfer_buffer transfer(pimpl->dev, total_bytes);

        // Upload newly loaded tiles
        for(auto& result : pimpl->pending_results)
        {
            auto vertices = std::vector<sdl::vertex_t2f_c4ub_v3f>(6);
            get_tile_vertices(result.key, 0.0F, 0.0F, 1.0F, 1.0F, vertices.data());

            sdl::buffer vbuf(pimpl->dev, sdl::buffer_usage::vertex, 6, sizeof(sdl::vertex_t2f_c4ub_v3f));
            pass.upload_buffer(transfer, vbuf, vertices);

            sdl::texture tex(pimpl->dev, result.surf);
            pass.upload_texture(transfer, tex, result.surf);

            auto gpu = std::make_shared<tile_gpu>(tile_gpu{result.key, std::move(vbuf), std::move(tex)});

            pimpl->cache.put(result.key, gpu);
        }
        pimpl->pending_results.clear();

        for(auto& fallback : fallbacks)
        {
            auto verts = std::vector<sdl::vertex_t2f_c4ub_v3f>(6);
            get_tile_vertices(fallback.key, fallback.u0, fallback.v0, fallback.u1, fallback.v1, verts.data());

            sdl::buffer vbuf(pimpl->dev, sdl::buffer_usage::vertex, 6, sizeof(sdl::vertex_t2f_c4ub_v3f));
            pass.upload_buffer(transfer, vbuf, verts);

            pimpl->fallback_quads.push_back({std::move(vbuf), std::move(fallback.resource)});
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
        for(const auto& key : pimpl->cache.visible_tiles())
        {
            auto gpu = pimpl->cache.find(key);
            if(!gpu)
            {
                continue;
            }

            pass.push_vertex_uniforms(0, &uniforms, sizeof(uniforms));
            pass.push_fragment_uniforms(0, &uniforms, sizeof(uniforms));
            pass.bind_vertex_buffer(gpu->vertex_buffer);
            pass.bind_fragment_texture_sampler(0, gpu->tex, pimpl->sampler);
            pass.draw(6);
        }

        // Render fallback quads (ancestor tiles with UV sub-rects)
        for(const auto& quad : pimpl->fallback_quads)
        {
            pass.push_vertex_uniforms(0, &uniforms, sizeof(uniforms));
            pass.push_fragment_uniforms(0, &uniforms, sizeof(uniforms));
            pass.bind_vertex_buffer(quad.vertex_buffer);
            pass.bind_fragment_texture_sampler(0, quad.ancestor_gpu->tex, pimpl->sampler);
            pass.draw(6);
        }
    }

} // namespace osect
