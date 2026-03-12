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

    class feature_renderer
    {
        struct impl;
        std::unique_ptr<impl> pimpl;

    public:
        feature_renderer(sdl::device& dev, const char* db_path);
        ~feature_renderer();

        // Call when viewport changes. Returns true if features need re-upload.
        bool update(double vx_min, double vy_min,
                    double vx_max, double vy_max,
                    int viewport_height, double aspect_ratio);

        bool needs_upload() const;
        void prepare(size_t& size) const;
        void copy(sdl::copy_pass& pass);
        void render(sdl::render_pass& pass, const render_context& ctx) const;
    };

} // namespace nasrbrowse
