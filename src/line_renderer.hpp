#pragma once

#include "line_style.hpp"
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
