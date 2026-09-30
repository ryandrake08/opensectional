#include "terrain_renderer.hpp"
#include "elevation_source.hpp"
#include "elevation_tile.hpp"
#include "map_view.hpp"
#include "program.hpp"
#include "render_context.hpp"
#include "tile_cache.hpp"
#include "tile_key.hpp"
#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <sdl/buffer.hpp>
#include <sdl/copy_pass.hpp>
#include <sdl/device.hpp>
#include <sdl/render_pass.hpp>
#include <sdl/sampler.hpp>
#include <sdl/surface.hpp>
#include <sdl/texture.hpp>
#include <sdl/transfer_buffer.hpp>
#include <sdl/types.hpp>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace osect
{
    namespace
    {
        void tile_vertices(const tile_key& key, float u0, float v0, float u1, float v1, float texture_offset,
                           float texture_scale, sdl::vertex_t2f_c4ub_v3f* vertices)
        {
            const auto [x0_m, y0_m, x1_m, y1_m] = tile_bounds_meters(key.x, key.y, key.z);
            const auto x0 = static_cast<float>(x0_m);
            const auto x1 = static_cast<float>(x1_m);
            const auto y0 = static_cast<float>(y0_m);
            const auto y1 = static_cast<float>(y1_m);
            constexpr uint8_t white = 255;
            u0 = texture_offset + u0 * texture_scale;
            v0 = texture_offset + v0 * texture_scale;
            u1 = texture_offset + u1 * texture_scale;
            v1 = texture_offset + v1 * texture_scale;
            vertices[0] = {u0, v1, white, white, white, white, x0, y0, 0.0F};
            vertices[1] = {u1, v1, white, white, white, white, x1, y0, 0.0F};
            vertices[2] = {u0, v0, white, white, white, white, x0, y1, 0.0F};
            vertices[3] = vertices[2];
            vertices[4] = vertices[1];
            vertices[5] = {u1, v0, white, white, white, white, x1, y1, 0.0F};
        }

        struct terrain_load_result
        {
            tile_key key;
            int width;
            int height;
            std::vector<uint16_t> elevations;
            // Water-class mask aligned to the height texels. Empty when
            // the tile has no water sidecar; the renderer then binds its
            // shared all-land texture instead.
            int water_width = 0;
            int water_height = 0;
            std::vector<uint8_t> water;
        };

        // Reads the R channel (the water class) of a mask PNG.
        std::vector<uint8_t> load_water_classes(const std::filesystem::path& path, int& width, int& height)
        {
            const sdl::surface surface(path.string().c_str());
            width = surface.width();
            height = surface.height();
            const auto* pixels = static_cast<const uint8_t*>(surface.pixels());
            std::vector<uint8_t> classes(static_cast<size_t>(width) * height);
            for(size_t i = 0; i < classes.size(); ++i)
            {
                classes[i] = pixels[i * 4];
            }
            return classes;
        }

        class terrain_loader
        {
            struct request
            {
                tile_key key;
                std::filesystem::path path;
                std::filesystem::path water_path; // empty when there is no water mask
            };

            mutable std::mutex mutex_;
            std::condition_variable cv_;
            std::deque<request> requests_;
            std::vector<terrain_load_result> results_;
            std::unordered_set<tile_key> pending_;
            std::unordered_set<tile_key> failed_;
            std::thread worker_;
            bool shutdown_ = false;
            bool failure_pending_ = false;

            void fail(const tile_key& key)
            {
                {
                    std::scoped_lock lock(mutex_);
                    pending_.erase(key);
                    failed_.insert(key);
                    failure_pending_ = true;
                }
                wake_main_thread();
            }

            void run()
            {
                while(true)
                {
                    request request;
                    {
                        std::unique_lock<std::mutex> lock(mutex_);
                        cv_.wait(lock, [this] { return shutdown_ || !requests_.empty(); });
                        if(shutdown_)
                        {
                            return;
                        }
                        request = std::move(requests_.front());
                        requests_.pop_front();
                    }

                    try
                    {
                        if(!std::filesystem::exists(request.path))
                        {
                            fail(request.key);
                            continue;
                        }
                        const auto tile = elevation_tile::load(request.path);
                        terrain_load_result result{request.key, tile.width(), tile.height(), tile.quantized_m(), 0, 0,
                                                   {}};
                        if(!request.water_path.empty() && std::filesystem::exists(request.water_path))
                        {
                            result.water =
                                load_water_classes(request.water_path, result.water_width, result.water_height);
                        }
                        {
                            std::scoped_lock lock(mutex_);
                            results_.push_back(std::move(result));
                        }
                        wake_main_thread();
                    }
                    catch(const std::exception&)
                    {
                        fail(request.key);
                    }
                }
            }

        public:
            terrain_loader() : worker_(&terrain_loader::run, this)
            {
            }

            terrain_loader(const terrain_loader&) = delete;
            terrain_loader& operator=(const terrain_loader&) = delete;
            terrain_loader(terrain_loader&&) = delete;
            terrain_loader& operator=(terrain_loader&&) = delete;

            ~terrain_loader()
            {
                {
                    std::scoped_lock lock(mutex_);
                    shutdown_ = true;
                }
                cv_.notify_one();
                worker_.join();
            }

            void request_tile(const tile_key& key, const std::filesystem::path& path,
                              const std::filesystem::path& water_path)
            {
                std::scoped_lock lock(mutex_);
                if(pending_.count(key) == 0 && failed_.count(key) == 0)
                {
                    pending_.insert(key);
                    requests_.push_back({key, path, water_path});
                    cv_.notify_one();
                }
            }

            void cancel()
            {
                std::scoped_lock lock(mutex_);
                for(const auto& request : requests_)
                {
                    pending_.erase(request.key);
                }
                requests_.clear();
            }

            bool failed(const tile_key& key) const
            {
                std::scoped_lock lock(mutex_);
                return failed_.count(key) != 0;
            }

            std::vector<terrain_load_result> drain()
            {
                std::scoped_lock lock(mutex_);
                for(const auto& result : results_)
                {
                    pending_.erase(result.key);
                }
                std::vector<terrain_load_result> result;
                result.swap(results_);
                return result;
            }

            bool drain_failures()
            {
                std::scoped_lock lock(mutex_);
                const bool failed = failure_pending_;
                failure_pending_ = false;
                return failed;
            }
        };

        struct terrain_gpu
        {
            std::unique_ptr<sdl::buffer> vertices;
            std::unique_ptr<sdl::texture> heights;
            std::unique_ptr<sdl::texture> water; // R8 class mask; null when the tile has no water sidecar
        };

        constexpr uint32_t ramp_width = 256;

        // Rasterises the hypsometric ramp into an RGBA row, walking elevation
        // linearly from the first stop to the last.
        std::vector<uint8_t> ramp_to_rgba(const std::vector<hypsometric_stop>& stops)
        {
            const float lo = stops.front().elevation_m;
            const float hi = stops.back().elevation_m;
            std::vector<uint8_t> pixels(static_cast<size_t>(ramp_width) * 4);
            size_t seg = 0;
            for(uint32_t x = 0; x < ramp_width; x++)
            {
                const float e = lo + (hi - lo) * (static_cast<float>(x) / (ramp_width - 1));
                while(seg + 2 < stops.size() && stops[seg + 1].elevation_m < e)
                {
                    seg++;
                }
                const hypsometric_stop& a = stops[seg];
                const hypsometric_stop& b = stops[seg + 1];
                const float span = b.elevation_m - a.elevation_m;
                const float f = span > 0.0F ? std::clamp((e - a.elevation_m) / span, 0.0F, 1.0F) : 0.0F;
                const auto channel = [f](float c0, float c1)
                { return static_cast<uint8_t>(std::lround((c0 + f * (c1 - c0)) * 255.0F)); };
                pixels[x * 4 + 0] = channel(a.r, b.r);
                pixels[x * 4 + 1] = channel(a.g, b.g);
                pixels[x * 4 + 2] = channel(a.b, b.b);
                pixels[x * 4 + 3] = 255;
            }
            return pixels;
        }
    }

    struct terrain_renderer::impl
    {
        sdl::device& dev;
        const elevation_source& source;
        terrain_style style;
        float cruise_altitude_ft = 0.0F;
        sdl::sampler sampler;
        sdl::sampler water_sampler; // nearest: the R8 value is an exact class index
        tile_cache<terrain_gpu> cache;
        terrain_loader loader;
        std::vector<terrain_load_result> pending;

        struct fallback_quad
        {
            sdl::buffer vertices;
            std::shared_ptr<terrain_gpu> ancestor;
            float texel_m = 0.0F;
        };
        std::vector<fallback_quad> fallbacks;
        bool fallback_dirty = false;

        std::unique_ptr<sdl::texture> ramp;
        bool ramp_uploaded = false;

        // Shared 1x1 R8 all-land mask, bound for every tile without a
        // water sidecar so the fragment sampler slot is always valid.
        std::unique_ptr<sdl::texture> no_water;
        bool no_water_uploaded = false;

        impl(sdl::device& dev, const elevation_source& source, terrain_style style)
            : dev(dev),
              source(source),
              style(std::move(style)),
              sampler(dev, sdl::filter::linear, sdl::filter::linear, sdl::sampler_address_mode::clamp_to_edge),
              water_sampler(dev, sdl::filter::nearest, sdl::filter::nearest, sdl::sampler_address_mode::clamp_to_edge),
              cache(source.min_zoom(), source.max_zoom(), source.tile_size(),
                    static_cast<std::size_t>(this->style.gpu_tile_cache)),
              ramp(std::make_unique<sdl::texture>(dev, ramp_width, 1U, sdl::texture_format::r8g8b8a8_unorm)),
              no_water(std::make_unique<sdl::texture>(dev, 1U, 1U, sdl::texture_format::r8_unorm))
        {
        }

        std::filesystem::path tile_path(const tile_key& key) const
        {
            const int count = 1 << key.z;
            const int x = (key.x % count + count) % count;
            return source.path() / std::to_string(key.z) / std::to_string(x) / (std::to_string(key.y) + ".png");
        }

        std::filesystem::path water_tile_path(const tile_key& key) const
        {
            if(!source.has_water_mask())
            {
                return {};
            }
            const int count = 1 << key.z;
            const int x = (key.x % count + count) % count;
            return source.path() / "water" / std::to_string(key.z) / std::to_string(x) /
                   (std::to_string(key.y) + ".png");
        }

        float texel_meters(const tile_key& key) const
        {
            const auto bounds = tile_bounds_meters(key.x, key.y, key.z);
            return static_cast<float>((bounds.x_max - bounds.x_min) / source.tile_size());
        }

        float texture_offset() const
        {
            return static_cast<float>(source.skirt()) / (source.tile_size() + 2.0F * source.skirt());
        }

        float texture_scale() const
        {
            return static_cast<float>(source.tile_size()) / (source.tile_size() + 2.0F * source.skirt());
        }

        void request(const tile_key& key)
        {
            if(cache.find(key))
            {
                return;
            }
            if(!loader.failed(key))
            {
                loader.request_tile(key, tile_path(key), water_tile_path(key));
                return;
            }
            if(key.z > source.min_zoom())
            {
                const int count = 1 << key.z;
                const int x = (key.x % count + count) % count;
                request({key.z - 1, x / 2, key.y / 2});
            }
        }
    };

    terrain_renderer::terrain_renderer(sdl::device& dev, const elevation_source& source, terrain_style style)
        : pimpl(std::make_unique<impl>(dev, source, std::move(style)))
    {
    }

    terrain_renderer::~terrain_renderer() = default;

    void terrain_renderer::update(double x_min, double y_min, double x_max, double y_max, int viewport_height)
    {
        pimpl->cache.update(
            x_min, y_min, x_max, y_max, viewport_height,
            [this]
            {
                pimpl->fallback_dirty = true;
                pimpl->loader.cancel();
            },
            [this](const tile_key& key) { pimpl->request(key); });
        for(const auto& key : pimpl->cache.visible_tiles())
        {
            pimpl->request(key);
        }
    }

    void terrain_renderer::set_cruise_altitude_ft(float altitude_ft)
    {
        pimpl->cruise_altitude_ft = altitude_ft;
    }

    void terrain_renderer::set_shading_mode(terrain_shading mode)
    {
        pimpl->style.mode = mode;
    }

    void terrain_renderer::drain()
    {
        if(pimpl->loader.drain_failures())
        {
            pimpl->fallback_dirty = true;
        }
        auto results = pimpl->loader.drain();
        if(!results.empty())
        {
            pimpl->fallback_dirty = true;
            pimpl->pending.insert(pimpl->pending.end(), std::make_move_iterator(results.begin()),
                                  std::make_move_iterator(results.end()));
        }
    }

    bool terrain_renderer::needs_upload() const
    {
        return !pimpl->pending.empty() || pimpl->fallback_dirty || !pimpl->ramp_uploaded || !pimpl->no_water_uploaded;
    }

    void terrain_renderer::copy(sdl::copy_pass& pass)
    {
        pimpl->fallbacks.clear();
        pimpl->fallback_dirty = false;
        constexpr uint32_t vertex_bytes = 6 * sizeof(sdl::vertex_t2f_c4ub_v3f);
        std::vector<typename tile_cache<terrain_gpu>::fallback> fallbacks;
        uint32_t bytes = 0;
        for(const auto& result : pimpl->pending)
        {
            bytes += vertex_bytes + static_cast<uint32_t>(result.elevations.size() * sizeof(uint16_t)) +
                     static_cast<uint32_t>(result.water.size());
        }
        for(const auto& key : pimpl->cache.visible_tiles())
        {
            if(pimpl->cache.find(key))
            {
                continue;
            }
            typename tile_cache<terrain_gpu>::fallback fallback;
            if(pimpl->cache.find_ancestor(key, fallback))
            {
                fallbacks.push_back(std::move(fallback));
                bytes += vertex_bytes;
            }
        }
        const bool upload_ramp = !pimpl->ramp_uploaded;
        if(upload_ramp)
        {
            bytes += ramp_width * 4;
        }
        const bool upload_no_water = !pimpl->no_water_uploaded;
        if(upload_no_water)
        {
            bytes += 1;
        }
        if(bytes == 0)
        {
            return;
        }

        sdl::transfer_buffer transfer(pimpl->dev, bytes);
        if(upload_ramp)
        {
            const auto pixels = ramp_to_rgba(pimpl->style.ramp);
            pass.upload_texture(transfer, *pimpl->ramp, pixels.data(), ramp_width, 1, ramp_width * 4);
            pimpl->ramp_uploaded = true;
        }
        if(upload_no_water)
        {
            const uint8_t land = 0;
            pass.upload_texture(transfer, *pimpl->no_water, &land, 1, 1, 1);
            pimpl->no_water_uploaded = true;
        }
        for(auto& result : pimpl->pending)
        {
            sdl::buffer vertices(pimpl->dev, sdl::buffer_usage::vertex, 6, sizeof(sdl::vertex_t2f_c4ub_v3f));
            std::vector<sdl::vertex_t2f_c4ub_v3f> quad(6);
            tile_vertices(result.key, 0.0F, 0.0F, 1.0F, 1.0F, pimpl->texture_offset(), pimpl->texture_scale(),
                          quad.data());
            pass.upload_buffer(transfer, vertices, quad);
            sdl::texture heights(pimpl->dev, static_cast<unsigned>(result.width), static_cast<unsigned>(result.height),
                                 sdl::texture_format::r16_unorm);
            pass.upload_texture(transfer, heights, result.elevations.data(), static_cast<uint32_t>(result.width),
                                static_cast<uint32_t>(result.height),
                                static_cast<uint32_t>(result.elevations.size() * sizeof(uint16_t)));
            auto gpu = std::make_shared<terrain_gpu>();
            gpu->vertices = std::make_unique<sdl::buffer>(std::move(vertices));
            gpu->heights = std::make_unique<sdl::texture>(std::move(heights));
            if(!result.water.empty())
            {
                sdl::texture water(pimpl->dev, static_cast<unsigned>(result.water_width),
                                   static_cast<unsigned>(result.water_height), sdl::texture_format::r8_unorm);
                pass.upload_texture(transfer, water, result.water.data(), static_cast<uint32_t>(result.water_width),
                                    static_cast<uint32_t>(result.water_height),
                                    static_cast<uint32_t>(result.water.size()));
                gpu->water = std::make_unique<sdl::texture>(std::move(water));
            }
            pimpl->cache.put(result.key, std::move(gpu));
        }
        pimpl->pending.clear();

        for(auto& fallback : fallbacks)
        {
            sdl::buffer vertices(pimpl->dev, sdl::buffer_usage::vertex, 6, sizeof(sdl::vertex_t2f_c4ub_v3f));
            std::vector<sdl::vertex_t2f_c4ub_v3f> quad(6);
            tile_vertices(fallback.key, fallback.u0, fallback.v0, fallback.u1, fallback.v1, pimpl->texture_offset(),
                          pimpl->texture_scale(), quad.data());
            pass.upload_buffer(transfer, vertices, quad);
            // fallback.key is the display tile; its texels span (u1-u0) of an
            // ancestor texel, so scale up to the ancestor's ground resolution.
            const float texel_m = pimpl->texel_meters(fallback.key) / (fallback.u1 - fallback.u0);
            pimpl->fallbacks.push_back({std::move(vertices), std::move(fallback.resource), texel_m});
        }
    }

    void terrain_renderer::render(sdl::render_pass& pass, const render_context& ctx, const glm::mat4& view_matrix) const
    {
        sdl::uniform_buffer uniforms;
        uniforms.projection_matrix = ctx.projection_matrix;
        uniforms.view_matrix = view_matrix;
        const int dim = pimpl->source.tile_size() + 2 * pimpl->source.skirt();
        uniforms.texture_size = glm::ivec2(dim, dim);

        // Shading parameters come from [terrain] and the active route tab.
        const terrain_style& style = pimpl->style;
        constexpr float ft_to_m = 0.3048F;
        uniforms.sun_azimuth = glm::radians(style.sun_azimuth_deg);
        uniforms.sun_altitude = glm::radians(style.sun_altitude_deg);
        uniforms.vertical_exaggeration = style.vertical_exaggeration;
        uniforms.terrain_opacity = style.opacity;
        uniforms.terrain_mode = static_cast<int>(style.mode);
        uniforms.hypso_min_m = style.ramp.front().elevation_m;
        uniforms.hypso_max_m = style.ramp.back().elevation_m;
        uniforms.taws_warning_m = style.cruise_warning_ft * ft_to_m;
        uniforms.taws_caution_m = style.cruise_caution_ft * ft_to_m;
        uniforms.taws_clear_m = style.cruise_clear_ft * ft_to_m;
        uniforms.taws_cruise_m = pimpl->cruise_altitude_ft * ft_to_m;
        uniforms.water_mode = pimpl->source.has_water_mask() ? 1 : 0;
        uniforms.water_r = style.water_r;
        uniforms.water_g = style.water_g;
        uniforms.water_b = style.water_b;

        const auto draw = [&pass, &uniforms, this](const terrain_gpu& gpu, const sdl::buffer& vertices, float texel_m)
        {
            uniforms.terrain_texel_m = texel_m;
            pass.push_vertex_uniforms(0, &uniforms, sizeof(uniforms));
            pass.push_fragment_uniforms(0, &uniforms, sizeof(uniforms));
            pass.bind_vertex_buffer(vertices);
            // Slots match the shader's first-use / declaration order.
            pass.bind_fragment_texture_sampler(0, *gpu.heights, pimpl->sampler);
            pass.bind_fragment_texture_sampler(1, gpu.water ? *gpu.water : *pimpl->no_water, pimpl->water_sampler);
            pass.bind_fragment_texture_sampler(2, *pimpl->ramp, pimpl->sampler);
            pass.draw(6);
        };
        for(const auto& key : pimpl->cache.visible_tiles())
        {
            if(const auto gpu = pimpl->cache.find(key))
            {
                draw(*gpu, *gpu->vertices, pimpl->texel_meters(key));
            }
        }
        for(const auto& fallback : pimpl->fallbacks)
        {
            draw(*fallback.ancestor, fallback.vertices, fallback.texel_m);
        }
    }
} // namespace osect
