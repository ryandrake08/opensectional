#pragma once

#include "feature_builder.hpp"
#include <glm/glm.hpp>
#include <memory>
#include <vector>

namespace sdl
{
    class device;
    class font;
    class text_engine;
    class copy_pass;
    class render_pass;
    class sampler;
}

namespace nasrbrowse
{
    struct render_context;

    class label_renderer
    {
        struct impl;
        std::unique_ptr<impl> pimpl;

    public:
        label_renderer(sdl::device& dev, sdl::text_engine& engine,
                       sdl::font& font, sdl::font& outline_font);
        ~label_renderer();

        // Set labels to render. Projects world-space positions to screen
        // using the current viewport transform.
        void set_labels(const std::vector<label_candidate>& labels,
                        double center_x, double center_y,
                        double half_extent_y,
                        int viewport_width, int viewport_height);

        bool needs_upload() const;
        void copy(sdl::copy_pass& pass, sdl::device& dev);
        void render(sdl::render_pass& pass, const render_context& ctx,
                    const sdl::sampler& samp,
                    int viewport_width, int viewport_height) const;
    };

} // namespace nasrbrowse
