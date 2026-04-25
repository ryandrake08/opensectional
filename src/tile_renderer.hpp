#pragma once

#include <glm/glm.hpp>
#include <memory>

namespace sdl
{
    class device;
    class copy_pass;
    class render_pass;
}

namespace osect
{
    struct render_context;

    class tile_renderer
    {
        struct impl;
        std::unique_ptr<impl> pimpl;

    public:
        tile_renderer(sdl::device& dev, const char* tile_path);
        ~tile_renderer();

        // Recompute visible tiles and enqueue background loads
        void update(double view_x_min, double view_y_min,
                    double view_x_max, double view_y_max,
                    double half_extent_y, int viewport_height,
                    double aspect_ratio);

        // Drain background loader into staging buffer
        void drain();

        // Check if tiles need upload
        bool needs_upload() const;

        // Upload tile data to GPU
        void copy(sdl::copy_pass& pass);

        // Render visible tiles
        void render(sdl::render_pass& pass, const render_context& ctx, const glm::mat4& view_matrix) const;
    };

} // namespace osect
