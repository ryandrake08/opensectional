#include "tile_renderer.hpp"
#include "program.hpp"
#include "render_context.hpp"
#include "tile_cache.hpp"
#include "tile_key.hpp"
#include "tile_loader.hpp"
#include "tile_quad.hpp"
#include "uniform_buffer.hpp"
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

    // A basemap tile image decoded on the loader thread.
    struct tile_load_result
    {
        tile_key key;
        sdl::surface surf;
    };
} // namespace osect

namespace osect
{

    struct tile_renderer::impl
    {
        sdl::device& dev;
        sdl::sampler sampler;
        tile_cache<tile_gpu> cache;

        // Background tile loader
        tile_loader<tile_load_result> loader;

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
              cache(0, 15, 256, 1024),
              loader([root = std::move(tile_path)](const tile_key& key)
                     { return tile_load_result{key, sdl::surface(tile_file_path(root, key).string().c_str())}; },
                     wake_main_thread)
        {
        }

        // Plan one copy() call: resolve fallback ancestors for the current
        // visible set and sum up every byte the transfer buffer will carry
        // (vertex buffers + tile texture pixels + fallback vertex buffers).
        std::pair<std::vector<typename tile_cache<tile_gpu>::fallback>, uint32_t> plan_uploads()
        {
            constexpr uint32_t vbuf_bytes = 6 * sizeof(sdl::vertex_t2f_c4ub_v3f);

            auto fallbacks = cache.fallbacks();

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

    void tile_renderer::update(double vx_min, double vy_min, double vx_max, double vy_max, int viewport_height)
    {
        if(pimpl->cache.update(vx_min, vy_min, vx_max, vy_max, viewport_height, pimpl->loader))
        {
            pimpl->fallback_dirty = true;
        }
    }

    void tile_renderer::drain()
    {
        auto results = pimpl->loader.drain();
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
            const auto vertices = tile_quad(result.key, 0.0F, 0.0F, 1.0F, 1.0F);
            sdl::buffer vbuf(pimpl->dev, sdl::buffer_usage::vertex, 6, sizeof(sdl::vertex_t2f_c4ub_v3f));
            pass.upload_buffer(transfer, vbuf, vertices.data());

            sdl::texture tex(pimpl->dev, result.surf);
            pass.upload_texture(transfer, tex, result.surf);

            auto gpu = std::make_shared<tile_gpu>(tile_gpu{result.key, std::move(vbuf), std::move(tex)});

            pimpl->cache.put(result.key, gpu);
        }
        pimpl->pending_results.clear();

        for(auto& fallback : fallbacks)
        {
            const auto vertices = tile_quad(fallback.key, fallback.u0, fallback.v0, fallback.u1, fallback.v1);
            sdl::buffer vbuf(pimpl->dev, sdl::buffer_usage::vertex, 6, sizeof(sdl::vertex_t2f_c4ub_v3f));
            pass.upload_buffer(transfer, vbuf, vertices.data());

            pimpl->fallback_quads.push_back({std::move(vbuf), std::move(fallback.resource)});
        }
    }

    void tile_renderer::render(sdl::render_pass& pass, const render_context& ctx, const glm::mat4& view_matrix) const
    {
        if(ctx.current_pass != render_pass_id::textured_trianglelist_0)
        {
            return;
        }

        auto uniforms = uniform_buffer{};
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
