#pragma once

#include <memory>

namespace sdl
{
    class device;
    class copy_pass;
    class render_pass;
}

namespace nasrbrowse
{
    struct render_context;
    struct layer_visibility;

    class feature_renderer
    {
        struct impl;
        std::unique_ptr<impl> pimpl;

    public:
        feature_renderer(sdl::device& dev, const char* db_path);
        ~feature_renderer();

        // Recompute visible features from database
        void update(double view_x_min, double view_y_min,
                    double view_x_max, double view_y_max,
                    int viewport_height, double aspect_ratio);

        // Check if features need upload
        bool needs_upload();

        // Upload feature data to GPU
        void copy(sdl::copy_pass& pass);

        // Render visible features
        void render(sdl::render_pass& pass, const render_context& ctx) const;

        // Set which feature layers are visible
        void set_visibility(const layer_visibility& vis);
    };

} // namespace nasrbrowse
