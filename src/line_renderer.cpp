#include "line_renderer.hpp"
#include <algorithm>
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
        uint32_t _pad;
        // total 80 bytes (5 * 16)
    };

    struct line_renderer::impl
    {
        // CPU-side data (pending upload)
        std::vector<std::vector<glm::vec2>> polylines;
        std::vector<line_style> styles;
        bool dirty = false;

        // GPU-side data
        std::unique_ptr<sdl::buffer> packed_points;
        std::unique_ptr<sdl::buffer> metadata_buf;
        uint32_t instance_count = 0;
    };

    line_renderer::line_renderer() : pimpl(new impl()) {}
    line_renderer::~line_renderer() = default;

    void line_renderer::set_polylines(std::vector<std::vector<glm::vec2>> polylines,
                                      std::vector<line_style> styles)
    {
        pimpl->polylines = std::move(polylines);
        pimpl->styles = std::move(styles);
        pimpl->dirty = true;
    }

    void line_renderer::clear()
    {
        pimpl->polylines.clear();
        pimpl->styles.clear();
        pimpl->packed_points.reset();
        pimpl->metadata_buf.reset();
        pimpl->instance_count = 0;
        pimpl->dirty = false;
    }

    bool line_renderer::needs_upload() const
    {
        return pimpl->dirty;
    }

    // Split large polylines into chunks to limit per-fragment shader work.
    // Each chunk overlaps by one point so segments connect continuously.
    static constexpr size_t max_chunk_points = 128;

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

            const line_style& style = pimpl->styles[i];

            // Process chunks of this polyline
            size_t offset = 0;
            while(offset < positions.size() - 1)
            {
                size_t end = std::min(offset + max_chunk_points, positions.size());
                size_t count = end - offset;

                // Compute bounds for this chunk
                glm::vec2 bmin = positions[offset];
                glm::vec2 bmax = positions[offset];
                for(size_t j = offset + 1; j < end; j++)
                {
                    bmin = glm::min(bmin, positions[j]);
                    bmax = glm::max(bmax, positions[j]);
                }

                // Record point offset before appending
                uint32_t point_offset = static_cast<uint32_t>(all_points.size());

                // Append points to packed buffer
                for(size_t j = offset; j < end; j++)
                {
                    all_points.emplace_back(positions[j].x, positions[j].y, 0.0F, 0.0F);
                }

                // Build metadata entry
                float effective_fill = style.fill_width > 0 ? style.fill_width : style.border_width;
                polyline_metadata_gpu meta {};
                meta.bounds_min_max = glm::vec4(bmin.x, bmin.y, bmax.x, bmax.y);
                meta.line_color = glm::vec4(style.r, style.g, style.b, style.a);
                meta.border_color = glm::vec4(0.0F, 0.0F, 0.0F, style.a);
                meta.line_half_width = style.line_width * 0.5F;
                meta.border_width = style.border_width;
                meta.dash_length = style.dash_length;
                meta.gap_length = style.gap_length;
                meta.fill_width = effective_fill;
                meta.segment_count = static_cast<uint32_t>(count - 1);
                meta.point_offset = point_offset;
                meta._pad = 0;
                all_metadata.push_back(meta);

                offset = end - 1; // overlap by one point
            }
        }

        pimpl->instance_count = static_cast<uint32_t>(all_metadata.size());

        if(pimpl->instance_count > 0)
        {
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

        glm::mat4 pv = projection * view;
        float vw = static_cast<float>(viewport_width);
        float vh = static_cast<float>(viewport_height);

        glm::vec2 w2s_scale(pv[0][0] * 0.5F * vw,
                            -pv[1][1] * 0.5F * vh);
        glm::vec2 w2s_offset((pv[3][0] + 1.0F) * 0.5F * vw,
                             (1.0F - pv[3][1]) * 0.5F * vh);

        shared_uniform_buffer uniforms {};
        uniforms.projection_matrix = projection;
        uniforms.view_matrix = view;
        uniforms.viewport_size = glm::vec2(vw, vh);
        uniforms.world_to_screen_scale = w2s_scale;
        uniforms.world_to_screen_offset = w2s_offset;

        pass.push_vertex_uniforms(0, &uniforms, sizeof(uniforms));
        pass.push_fragment_uniforms(0, &uniforms, sizeof(uniforms));
        pass.bind_vertex_storage_buffer(0, *pimpl->metadata_buf);
        pass.bind_fragment_storage_buffer(0, *pimpl->packed_points);
        pass.bind_fragment_storage_buffer(1, *pimpl->metadata_buf);
        pass.draw(6, pimpl->instance_count);
    }

} // namespace nasrbrowse
