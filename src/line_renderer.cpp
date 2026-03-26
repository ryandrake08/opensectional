#include "line_renderer.hpp"
#include <algorithm>
#include <sdl/buffer.hpp>
#include <sdl/copy_pass.hpp>
#include <sdl/render_pass.hpp>
#include <sdl/types.hpp>

namespace nasrbrowse
{
    // Uniform buffer layout matching line.hlsl cbuffer
    struct line_uniform_buffer
    {
        glm::mat4 projection_matrix;    // 64 bytes, offset 0
        glm::mat4 view_matrix;          // 64 bytes, offset 64
        glm::vec4 bounds_min_max;       // 16 bytes, offset 128 (xy=min, zw=max)
        glm::vec4 line_color;           // 16 bytes, offset 144
        glm::vec4 border_color;         // 16 bytes, offset 160
        glm::vec2 viewport_size;        // 8 bytes, offset 176
        glm::vec2 world_to_screen_scale;  // 8 bytes, offset 184
        glm::vec2 world_to_screen_offset; // 8 bytes, offset 192
        float line_half_width;          // 4 bytes, offset 200
        float border_width;             // 4 bytes, offset 204
        float dash_length;              // 4 bytes, offset 208
        float gap_length;               // 4 bytes, offset 212
        uint32_t segment_count;         // 4 bytes, offset 216
        uint32_t _pad;                  // 4 bytes, offset 220
    };

    struct polyline_gpu
    {
        std::unique_ptr<sdl::buffer> storage;
        glm::vec2 bounds_min;
        glm::vec2 bounds_max;
        uint32_t segment_count;
        line_style style;
    };

    struct line_renderer::impl
    {
        // CPU-side data (pending upload)
        std::vector<std::vector<glm::vec2>> polylines;
        std::vector<line_style> styles;
        bool dirty = false;

        // GPU-side data
        std::vector<polyline_gpu> gpu_polylines;
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
        pimpl->gpu_polylines.clear();
        pimpl->dirty = false;
    }

    bool line_renderer::needs_upload() const
    {
        return pimpl->dirty;
    }

    // Split large polylines into chunks to limit per-fragment shader work.
    // Each chunk overlaps by one point so segments connect continuously.
    static constexpr size_t max_chunk_points = 128;

    static void upload_chunk(sdl::copy_pass& pass, const sdl::device& dev,
                             const glm::vec2* points, size_t count,
                             const line_style& style,
                             std::vector<polyline_gpu>& out)
    {
        if(count < 2)
        {
            return;
        }

        glm::vec2 bmin = points[0];
        glm::vec2 bmax = points[0];
        for(size_t j = 1; j < count; j++)
        {
            bmin = glm::min(bmin, points[j]);
            bmax = glm::max(bmax, points[j]);
        }

        std::vector<glm::vec4> buffer_data;
        buffer_data.reserve(count);
        for(size_t j = 0; j < count; j++)
        {
            buffer_data.emplace_back(points[j].x, points[j].y, 0.0F, 0.0F);
        }

        auto storage = pass.create_and_upload_buffer(
            dev, sdl::buffer_usage::graphics_storage_read, buffer_data);

        polyline_gpu gpu;
        gpu.storage = std::make_unique<sdl::buffer>(std::move(storage));
        gpu.bounds_min = bmin;
        gpu.bounds_max = bmax;
        gpu.segment_count = static_cast<uint32_t>(count - 1);
        gpu.style = style;
        out.push_back(std::move(gpu));
    }

    void line_renderer::copy(sdl::copy_pass& pass, const sdl::device& dev)
    {
        if(!pimpl->dirty)
        {
            return;
        }

        pimpl->gpu_polylines.clear();

        for(size_t i = 0; i < pimpl->polylines.size(); i++)
        {
            const auto& positions = pimpl->polylines[i];
            if(positions.size() < 2)
            {
                continue;
            }

            if(positions.size() <= max_chunk_points)
            {
                upload_chunk(pass, dev, positions.data(), positions.size(),
                             pimpl->styles[i], pimpl->gpu_polylines);
            }
            else
            {
                // Split into overlapping chunks of max_chunk_points
                size_t offset = 0;
                while(offset < positions.size() - 1)
                {
                    size_t end = std::min(offset + max_chunk_points, positions.size());
                    upload_chunk(pass, dev, positions.data() + offset, end - offset,
                                 pimpl->styles[i], pimpl->gpu_polylines);
                    offset = end - 1; // overlap by one point for segment continuity
                }
            }
        }

        pimpl->dirty = false;
    }

    void line_renderer::render(sdl::render_pass& pass,
                               const glm::mat4& projection,
                               const glm::mat4& view,
                               int viewport_width,
                               int viewport_height) const
    {
        if(pimpl->gpu_polylines.empty())
        {
            return;
        }

        // Compute world-to-screen transform from proj*view matrices
        // For ortho 2D: screen.x = (ndc.x + 1) * 0.5 * vw
        //               screen.y = (1 - ndc.y) * 0.5 * vh
        glm::mat4 pv = projection * view;
        float vw = static_cast<float>(viewport_width);
        float vh = static_cast<float>(viewport_height);

        // Extract 2D affine transform components from the combined matrix
        // ndc.x = pv[0][0] * wx + pv[3][0]  (assuming pv[1][0]=0, pv[2][0]=0)
        // ndc.y = pv[1][1] * wy + pv[3][1]  (assuming pv[0][1]=0, pv[2][1]=0)
        glm::vec2 w2s_scale(pv[0][0] * 0.5F * vw,
                            -pv[1][1] * 0.5F * vh);
        glm::vec2 w2s_offset((pv[3][0] + 1.0F) * 0.5F * vw,
                             (1.0F - pv[3][1]) * 0.5F * vh);

        // Compute viewport bounds in world space for culling.
        // screen = world * w2s_scale + w2s_offset, so:
        // world = (screen - w2s_offset) / w2s_scale
        float world_x0 = -w2s_offset.x / w2s_scale.x;
        float world_x1 = (vw - w2s_offset.x) / w2s_scale.x;
        float world_y0 = -w2s_offset.y / w2s_scale.y;
        float world_y1 = (vh - w2s_offset.y) / w2s_scale.y;
        if(world_x0 > world_x1) std::swap(world_x0, world_x1);
        if(world_y0 > world_y1) std::swap(world_y0, world_y1);

        for(const auto& gpu : pimpl->gpu_polylines)
        {
            float margin_pixels = gpu.style.line_width * 0.5F + gpu.style.border_width;
            float margin_x = margin_pixels / std::abs(w2s_scale.x);
            float margin_y = margin_pixels / std::abs(w2s_scale.y);

            // Skip polylines entirely outside the viewport
            if(gpu.bounds_max.x + margin_x < world_x0 ||
               gpu.bounds_min.x - margin_x > world_x1 ||
               gpu.bounds_max.y + margin_y < world_y0 ||
               gpu.bounds_min.y - margin_y > world_y1)
            {
                continue;
            }

            line_uniform_buffer uniforms;
            uniforms.projection_matrix = projection;
            uniforms.view_matrix = view;
            uniforms.bounds_min_max = glm::vec4(
                gpu.bounds_min.x - margin_x, gpu.bounds_min.y - margin_y,
                gpu.bounds_max.x + margin_x, gpu.bounds_max.y + margin_y);
            uniforms.line_color = glm::vec4(
                gpu.style.r, gpu.style.g, gpu.style.b, gpu.style.a);
            uniforms.border_color = glm::vec4(0.0F, 0.0F, 0.0F, gpu.style.a);
            uniforms.viewport_size = glm::vec2(vw, vh);
            uniforms.world_to_screen_scale = w2s_scale;
            uniforms.world_to_screen_offset = w2s_offset;
            uniforms.line_half_width = gpu.style.line_width * 0.5F;
            uniforms.border_width = gpu.style.border_width;
            uniforms.dash_length = gpu.style.dash_length;
            uniforms.gap_length = gpu.style.gap_length;
            uniforms.segment_count = gpu.segment_count;
            uniforms._pad = 0;

            pass.push_vertex_uniforms(0, &uniforms, sizeof(uniforms));
            pass.push_fragment_uniforms(0, &uniforms, sizeof(uniforms));
            pass.bind_fragment_storage_buffer(0, *gpu.storage);
            pass.draw(6);
        }
    }

} // namespace nasrbrowse
