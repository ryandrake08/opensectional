#pragma once

#include "terrain_style.hpp"

#include <glm/glm.hpp>
#include <memory>

namespace sdl
{
    class copy_pass;
    class device;
    class render_pass;
}

namespace osect
{
    class elevation_source;
    struct render_context;

    class terrain_renderer
    {
        struct impl;
        std::unique_ptr<impl> pimpl;

    public:
        terrain_renderer(sdl::device& dev, const elevation_source& source, terrain_style style);
        ~terrain_renderer();

        void update(double view_x_min, double view_y_min, double view_x_max, double view_y_max, int viewport_height);
        void drain();
        bool needs_upload() const;
        void copy(sdl::copy_pass& pass);
        void set_cruise_altitude_ft(float altitude_ft);
        void set_shading_mode(terrain_shading mode);
        void render(sdl::render_pass& pass, const render_context& ctx, const glm::mat4& view_matrix) const;
    };
} // namespace osect
