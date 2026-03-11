#include "tile_renderer.hpp"
#include "lru_set.hpp"
#include "map_view.hpp"
#include "render_context.hpp"
#include <algorithm>
#include <cmath>
#include <glm/ext/matrix_transform.hpp>
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
    struct tile_key
    {
        int z, x, y;

        bool operator==(const tile_key& other) const
        {
            return z == other.z && x == other.x && y == other.y;
        }
    };

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
    struct hash<nasrbrowse::tile_key>
    {
        size_t operator()(const nasrbrowse::tile_key& k) const
        {
            size_t h = 0;
            h ^= std::hash<int>()(k.z) + 0x9e3779b9 + (h << 6) + (h >> 2);
            h ^= std::hash<int>()(k.x) + 0x9e3779b9 + (h << 6) + (h >> 2);
            h ^= std::hash<int>()(k.y) + 0x9e3779b9 + (h << 6) + (h >> 2);
            return h;
        }
    };

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

        // Current view state (for view matrix computation)
        double center_x, center_y, half_extent_y;

        // Cache: tile_key -> weak_ptr to GPU resources
        std::unordered_map<tile_key, std::weak_ptr<tile_gpu>> tile_map;

        // LRU cache owns the shared_ptrs, eviction frees GPU resources
        lru_set<std::shared_ptr<tile_gpu>> cache;

        // Tiles needing upload this frame
        std::vector<tile_key> tiles_needing_upload;

        // Surfaces loaded during prepare(), consumed in copy()
        mutable std::vector<std::pair<tile_key, sdl::surface>> surfaces_for_upload;

        impl(sdl::device& dev, const std::string& tile_path)
            : dev(dev)
            , tile_path(tile_path)
            , max_zoom(15)
            , current_zoom(0)
            , center_x(0)
            , center_y(0)
            , half_extent_y(HALF_CIRCUMFERENCE)
            , cache(256)
        {
        }

        std::string tile_file_path(const tile_key& key) const
        {
            return tile_path + "/" + std::to_string(key.z) + "/" +
                   std::to_string(key.x) + "/" + std::to_string(key.y) + ".png";
        }

        bool tile_file_exists(const tile_key& key) const
        {
            FILE* f = fopen(tile_file_path(key).c_str(), "r");
            if(f)
            {
                fclose(f);
                return true;
            }
            return false;
        }
    };

    tile_renderer::tile_renderer(sdl::device& dev, const char* tile_path)
        : pimpl(new impl(dev, tile_path))
    {
    }

    tile_renderer::~tile_renderer() = default;

    void tile_renderer::update(double vx_min, double vy_min,
                               double vx_max, double vy_max,
                               int viewport_height, double)
    {
        pimpl->center_x = (vx_min + vx_max) * 0.5;
        pimpl->center_y = (vy_min + vy_max) * 0.5;
        pimpl->half_extent_y = (vy_max - vy_min) * 0.5;

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

        tx_min = std::max(0, tx_min);
        tx_max = std::min(n - 1, tx_max);
        ty_min = std::max(0, ty_min);
        ty_max = std::min(n - 1, ty_max);

        pimpl->visible_tiles.clear();
        pimpl->tiles_needing_upload.clear();

        for(int ty = ty_min; ty <= ty_max; ty++)
        {
            for(int tx = tx_min; tx <= tx_max; tx++)
            {
                tile_key key { zoom, tx, ty };
                pimpl->visible_tiles.push_back(key);

                auto it = pimpl->tile_map.find(key);
                if(it == pimpl->tile_map.end() || it->second.expired())
                {
                    pimpl->tiles_needing_upload.push_back(key);
                }
            }
        }

    }

    bool tile_renderer::needs_upload() const
    {
        return !pimpl->tiles_needing_upload.empty();
    }

    void tile_renderer::prepare(size_t& size) const
    {
        pimpl->surfaces_for_upload.clear();

        for(const auto& key : pimpl->tiles_needing_upload)
        {
            if(!pimpl->tile_file_exists(key))
            {
                continue;
            }

            try
            {
                sdl::surface surf(pimpl->tile_file_path(key).c_str());
                size += 6 * sizeof(sdl::vertex_t2f_c4ub_v3f);
                size += surf.size();
                pimpl->surfaces_for_upload.emplace_back(key, std::move(surf));
            }
            catch(...)
            {
                // Skip tiles that fail to load
            }
        }
    }

    void tile_renderer::copy(sdl::copy_pass& pass)
    {
        for(auto& [key, surf] : pimpl->surfaces_for_upload)
        {
            std::vector<sdl::vertex_t2f_c4ub_v3f> vertices(6);
            get_tile_vertices(key, vertices.data());

            auto gpu = std::make_shared<tile_gpu>();
            gpu->key = key;

            auto vbuf = pass.create_and_upload_buffer(pimpl->dev, sdl::buffer_usage::vertex, vertices);
            gpu->vertex_buffer = std::make_unique<sdl::buffer>(std::move(vbuf));

            auto tex = pass.create_and_upload_texture(pimpl->dev, surf);
            gpu->tex = std::make_unique<sdl::texture>(std::move(tex));

            pimpl->cache.put(gpu);
            pimpl->tile_map[key] = gpu;
        }

        pimpl->surfaces_for_upload.clear();
        pimpl->tiles_needing_upload.clear();
    }

    void tile_renderer::render(sdl::render_pass& pass, const render_context& ctx) const
    {
        if(ctx.current_pass != render_pass_id::textured_trianglelist_0)
        {
            return;
        }

        // View matrix: transform from meters to NDC
        // NDC range: x in [-aspect*0.5, aspect*0.5], y in [-0.5, 0.5]
        // Scale: 1 / (2 * half_extent_y) maps the viewport height to [-0.5, 0.5]
        float s = static_cast<float>(1.0 / (2.0 * pimpl->half_extent_y));
        float cx = static_cast<float>(pimpl->center_x);
        float cy = static_cast<float>(pimpl->center_y);

        glm::mat4 view_matrix = glm::scale(glm::mat4(1.0F), glm::vec3(s, s, 1.0F)) *
                                 glm::translate(glm::mat4(1.0F), glm::vec3(-cx, -cy, 0.0F));

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
