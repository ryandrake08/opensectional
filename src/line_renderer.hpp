#pragma once

#include <glm/glm.hpp>
#include <memory>
#include <vector>

namespace sdl
{
    class copy_pass;
    class device;
    class render_pass;
}

namespace nasrbrowse
{

    struct line_style
    {
        float line_width;   // total width in pixels
        float border_width; // border width in pixels (outside of path)
        float dash_length;  // dash length in pixels (0 = solid)
        float gap_length;   // gap length in pixels
        float r, g, b, a;  // line color (border is always black)
        float fill_width;   // border width on inside of path (0 = same as border_width)
    };

    // Circle primitive (center + radius in world-space Mercator meters)
    struct circle_data
    {
        glm::vec2 center;
        float radius;
        line_style style;
    };

    class line_renderer
    {
        struct impl;
        std::unique_ptr<impl> pimpl;

    public:
        line_renderer();
        ~line_renderer();

        line_renderer(const line_renderer&) = delete;
        line_renderer& operator=(const line_renderer&) = delete;

        // Set geometry data: polylines and/or circles in world-space.
        void set_data(std::vector<std::vector<glm::vec2>> polylines,
                      std::vector<line_style> styles,
                      std::vector<circle_data> circles);

        // Clear all polyline data.
        void clear();

        bool needs_upload() const;
        void copy(sdl::copy_pass& pass, const sdl::device& dev);

        // Render all polylines. Line pipeline must already be bound.
        void render(sdl::render_pass& pass,
                    const glm::mat4& projection,
                    const glm::mat4& view,
                    int viewport_width,
                    int viewport_height) const;
    };

} // namespace nasrbrowse
