#include "line_renderer.hpp"
#include <algorithm>
#include <memory>
#include <sdl/buffer.hpp>
#include <sdl/copy_pass.hpp>
#include <sdl/render_pass.hpp>
#include <sdl/types.hpp>

namespace nasrbrowse
{
    // Shared uniform buffer (same for all instances)
    struct shared_uniform_buffer
    {
        glm::mat4 projection_matrix;      // 64 bytes, offset 0
        glm::mat4 view_matrix;            // 64 bytes, offset 64
        glm::vec2 viewport_size;           // 8 bytes, offset 128
        glm::vec2 world_to_screen_scale;   // 8 bytes, offset 136
        glm::vec2 world_to_screen_offset;  // 8 bytes, offset 144
        glm::vec2 _pad0;                   // 8 bytes, offset 152
        // total 160 bytes (10 * 16)
    };

    // Primitive types for instanced rendering
    constexpr uint32_t PRIMITIVE_POLYLINE = 0;
    constexpr uint32_t PRIMITIVE_CIRCLE = 1;

    // Per-instance metadata (must match PolylineMetadata in line.hlsl)
    struct polyline_metadata_gpu
    {
        glm::vec4 bounds_min_max;     // xy=min, zw=max (no margin)
        glm::vec4 line_color;
        glm::vec4 border_color;
        float line_half_width;
        float border_width;
        float dash_length;
        float gap_length;
        float fill_width;
        uint32_t segment_count;
        uint32_t point_offset;
        uint32_t primitive_type;      // 0=polyline, 1=circle
        float circle_center_x;       // world-space Mercator
        float circle_center_y;
        float circle_radius;          // world-space Mercator
        float _pad2;
        // total 96 bytes (6 * 16)
    };

    struct line_renderer::impl
    {
        // CPU-side data (pending upload)
        std::vector<std::vector<glm::vec2>> polylines;
        std::vector<line_style> styles;
        std::vector<circle_data> circles;
        bool dirty = false;

        // GPU-side data
        std::unique_ptr<sdl::buffer> packed_points;
        std::unique_ptr<sdl::buffer> metadata_buf;
        uint32_t instance_count = 0;
    };

    line_renderer::line_renderer() : pimpl(std::make_unique<impl>()) {}
    line_renderer::~line_renderer() = default;

    void line_renderer::set_data(std::vector<std::vector<glm::vec2>> polylines,
                                  std::vector<line_style> styles,
                                  std::vector<circle_data> circles)
    {
        pimpl->polylines = std::move(polylines);
        pimpl->styles = std::move(styles);
        pimpl->circles = std::move(circles);
        pimpl->dirty = true;
    }

    void line_renderer::clear()
    {
        pimpl->polylines.clear();
        pimpl->styles.clear();
        pimpl->circles.clear();
        pimpl->packed_points.reset();
        pimpl->metadata_buf.reset();
        pimpl->instance_count = 0;
        pimpl->dirty = false;
    }

    bool line_renderer::needs_upload() const
    {
        return pimpl->dirty;
    }

    void line_renderer::copy(sdl::copy_pass& pass, const sdl::device& dev)
    {
        if(!pimpl->dirty)
        {
            return;
        }

        std::vector<glm::vec4> all_points;
        std::vector<polyline_metadata_gpu> all_metadata;

        for(size_t i = 0; i < pimpl->polylines.size(); i++)
        {
            const auto& positions = pimpl->polylines[i];
            if(positions.size() < 2)
            {
                continue;
            }

            const auto& style = pimpl->styles[i];

            // Compute bounds
            auto bmin = positions[0];
            auto bmax = positions[0];
            for(size_t j = 1; j < positions.size(); j++)
            {
                bmin = glm::min(bmin, positions[j]);
                bmax = glm::max(bmax, positions[j]);
            }

            auto point_offset = static_cast<uint32_t>(all_points.size());

            for(const auto& p : positions)
            {
                all_points.emplace_back(p.x, p.y, 0.0F, 0.0F);
            }

            auto effective_fill = style.fill_width > 0 ? style.fill_width : style.border_width;
            auto meta = polyline_metadata_gpu{};
            meta.bounds_min_max = glm::vec4(bmin.x, bmin.y, bmax.x, bmax.y);
            meta.line_color = glm::vec4(style.r, style.g, style.b, style.a);
            meta.border_color = glm::vec4(0.0F, 0.0F, 0.0F, style.a);
            meta.line_half_width = style.line_width * 0.5F;
            meta.border_width = style.border_width;
            meta.dash_length = style.dash_length;
            meta.gap_length = style.gap_length;
            meta.fill_width = effective_fill;
            meta.segment_count = static_cast<uint32_t>(positions.size() - 1);
            meta.point_offset = point_offset;
            meta.primitive_type = PRIMITIVE_POLYLINE;
            all_metadata.push_back(meta);
        }

        // Add circle instances
        for(const auto& c : pimpl->circles)
        {
            auto effective_fill = c.style.fill_width > 0 ? c.style.fill_width : c.style.border_width;
            auto meta = polyline_metadata_gpu{};
            meta.bounds_min_max = glm::vec4(
                c.center.x - c.radius, c.center.y - c.radius,
                c.center.x + c.radius, c.center.y + c.radius);
            meta.line_color = glm::vec4(c.style.r, c.style.g, c.style.b, c.style.a);
            meta.border_color = glm::vec4(0.0F, 0.0F, 0.0F, c.style.a);
            meta.line_half_width = c.style.line_width * 0.5F;
            meta.border_width = c.style.border_width;
            meta.dash_length = c.style.dash_length;
            meta.gap_length = c.style.gap_length;
            meta.fill_width = effective_fill;
            meta.segment_count = 0;
            meta.point_offset = 0;
            meta.primitive_type = PRIMITIVE_CIRCLE;
            meta.circle_center_x = c.center.x;
            meta.circle_center_y = c.center.y;
            meta.circle_radius = c.radius;
            all_metadata.push_back(meta);
        }

        pimpl->instance_count = static_cast<uint32_t>(all_metadata.size());

        if(pimpl->instance_count > 0)
        {
            // Ensure points buffer is non-empty (circles don't use it but it must be bound)
            if(all_points.empty())
            {
                all_points.emplace_back(0.0F, 0.0F, 0.0F, 0.0F);
            }

            auto pts = pass.create_and_upload_buffer(
                dev, sdl::buffer_usage::graphics_storage_read, all_points);
            pimpl->packed_points = std::make_unique<sdl::buffer>(std::move(pts));

            auto meta = pass.create_and_upload_buffer(
                dev, sdl::buffer_usage::graphics_storage_read, all_metadata);
            pimpl->metadata_buf = std::make_unique<sdl::buffer>(std::move(meta));
        }
        else
        {
            pimpl->packed_points.reset();
            pimpl->metadata_buf.reset();
        }

        pimpl->dirty = false;
    }

    void line_renderer::render(sdl::render_pass& pass,
                               const glm::mat4& projection,
                               const glm::mat4& view,
                               int viewport_width,
                               int viewport_height) const
    {
        if(pimpl->instance_count == 0)
        {
            return;
        }

        auto pv = projection * view;
        auto vw = static_cast<float>(viewport_width);
        auto vh = static_cast<float>(viewport_height);

        glm::vec2 w2s_scale(pv[0][0] * 0.5F * vw,
                            -pv[1][1] * 0.5F * vh);
        glm::vec2 w2s_offset((pv[3][0] + 1.0F) * 0.5F * vw,
                             (1.0F - pv[3][1]) * 0.5F * vh);

        auto uniforms = shared_uniform_buffer{};
        uniforms.projection_matrix = projection;
        uniforms.view_matrix = view;
        uniforms.viewport_size = glm::vec2(vw, vh);
        uniforms.world_to_screen_scale = w2s_scale;
        uniforms.world_to_screen_offset = w2s_offset;

        pass.push_vertex_uniforms(0, &uniforms, sizeof(uniforms));
        pass.push_fragment_uniforms(0, &uniforms, sizeof(uniforms));
        pass.bind_vertex_storage_buffer(0, *pimpl->metadata_buf);
        pass.bind_vertex_storage_buffer(1, *pimpl->packed_points);
        pass.bind_fragment_storage_buffer(0, *pimpl->packed_points);
        pass.bind_fragment_storage_buffer(1, *pimpl->metadata_buf);
        pass.draw(6, pimpl->instance_count);
    }

} // namespace nasrbrowse
