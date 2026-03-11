#pragma once

#include <cstddef>
#include <memory>

namespace sdl
{
    class device;
    class copy_pass;
    class render_pass;
    class sampler;
}

namespace nasrbrowse
{
    struct render_context;

    class tile_renderer
    {
        struct impl;
        std::unique_ptr<impl> pimpl;

    public:
        tile_renderer(sdl::device& dev, const char* tile_path);
        ~tile_renderer();

        // Call when viewport changes to recompute visible tiles
        void update(double view_x_min, double view_y_min,
                    double view_x_max, double view_y_max,
                    int viewport_height, double aspect_ratio);

        // Accumulate transfer buffer size needed
        void prepare(size_t& size) const;

        // Upload tile data to GPU
        void copy(sdl::copy_pass& pass);

        // Render visible tiles
        void render(sdl::render_pass& pass, const render_context& ctx) const;

        // Whether there are tiles needing upload
        bool needs_upload() const;
    };

} // namespace nasrbrowse
