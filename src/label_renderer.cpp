#include "label_renderer.hpp"
#include "render_context.hpp"
#include <algorithm>
#include <glm/ext/matrix_clip_space.hpp>
#include <sdl/buffer.hpp>
#include <sdl/copy_pass.hpp>
#include <sdl/device.hpp>
#include <sdl/font.hpp>
#include <sdl/render_pass.hpp>
#include <sdl/sampler.hpp>
#include <sdl/text.hpp>
#include <sdl/text_engine.hpp>
#include <sdl/types.hpp>
#include <vector>

namespace nasrbrowse
{
    struct label_renderer::impl
    {
        sdl::device& dev;
        sdl::text_engine& engine;
        sdl::font& font;
        sdl::font& outline_font;

        // Cached text objects (reused across frames)
        std::vector<sdl::text> fill_texts;
        std::vector<sdl::text> outline_texts;

        // Screen positions (Y-up, for projection)
        std::vector<glm::vec3> positions;

        // GPU buffers for fill and outline
        std::vector<sdl::vertex_t2f_c4ub_v3f> fill_vertices;
        std::vector<int> fill_indices;
        std::unique_ptr<sdl::buffer> fill_vertex_buffer;
        std::unique_ptr<sdl::buffer> fill_index_buffer;

        std::vector<sdl::vertex_t2f_c4ub_v3f> outline_vertices;
        std::vector<int> outline_indices;
        std::unique_ptr<sdl::buffer> outline_vertex_buffer;
        std::unique_ptr<sdl::buffer> outline_index_buffer;

        bool dirty = false;

        impl(sdl::device& dev, sdl::text_engine& engine,
             sdl::font& font, sdl::font& outline_font)
            : dev(dev)
            , engine(engine)
            , font(font)
            , outline_font(outline_font)
        {
        }
    };

    label_renderer::label_renderer(sdl::device& dev, sdl::text_engine& engine,
                                   sdl::font& font, sdl::font& outline_font)
        : pimpl(new impl(dev, engine, font, outline_font))
    {
    }

    label_renderer::~label_renderer() = default;

    void label_renderer::set_labels(const std::vector<label_candidate>& labels,
                                    double center_x, double center_y,
                                    double half_extent_y,
                                    int viewport_width, int viewport_height)
    {
        constexpr float APPROX_CHAR_WIDTH = 7.5F;
        constexpr float LABEL_HEIGHT = 16.0F;
        constexpr float LABEL_OFFSET_Y = 24.0F;
        constexpr float LABEL_PAD_X = 4.0F;
        constexpr float LABEL_PAD_Y = 2.0F;

        pimpl->fill_texts.clear();
        pimpl->outline_texts.clear();
        pimpl->positions.clear();

        // World-to-screen transform
        double scale = viewport_height / (2.0 * half_extent_y);
        double screen_cx = viewport_width * 0.5;
        double screen_cy = viewport_height * 0.5;

        // Sort by priority (descending). Work on a copy to avoid
        // mutating the feature_renderer's cached vector.
        std::vector<const label_candidate*> sorted;
        sorted.reserve(labels.size());
        for(const auto& lc : labels)
        {
            sorted.push_back(&lc);
        }
        std::sort(sorted.begin(), sorted.end(),
            [](const label_candidate* a, const label_candidate* b)
            {
                return a->priority > b->priority;
            });

        // Greedy overlap elimination and projection
        struct placed_rect { float x0, y0, x1, y1; };
        std::vector<placed_rect> placed;
        placed.reserve(sorted.size());

        for(const auto* lc : sorted)
        {
            // Project world to screen (Y-down)
            float sx = static_cast<float>(
                (lc->mx - center_x) * scale + screen_cx);
            float sy = static_cast<float>(
                screen_cy - (lc->my - center_y) * scale);

            // Label dimensions and position (above symbol, centered)
            float label_w = static_cast<float>(lc->text.size()) * APPROX_CHAR_WIDTH;
            float lx = sx - label_w * 0.5F;
            float ly = sy - LABEL_OFFSET_Y - LABEL_HEIGHT;

            // Bounding box with padding
            float x0 = lx - LABEL_PAD_X;
            float y0 = ly - LABEL_PAD_Y;
            float x1 = lx + label_w + LABEL_PAD_X;
            float y1 = ly + LABEL_HEIGHT + LABEL_PAD_Y;

            // Skip if off-screen
            if(x1 < 0 || x0 > viewport_width || y1 < 0 || y0 > viewport_height)
            {
                continue;
            }

            // Check overlap with already-placed labels
            bool overlaps = false;
            for(const auto& p : placed)
            {
                if(x0 < p.x1 && x1 > p.x0 && y0 < p.y1 && y1 > p.y0)
                {
                    overlaps = true;
                    break;
                }
            }
            if(overlaps) continue;

            placed.push_back({x0, y0, x1, y1});

            // Convert to projection Y-up
            float proj_y = static_cast<float>(viewport_height) - (sy - LABEL_OFFSET_Y);

            pimpl->fill_texts.emplace_back(
                pimpl->engine, pimpl->font, lc->text.c_str());
            pimpl->outline_texts.emplace_back(
                pimpl->engine, pimpl->outline_font, lc->text.c_str());
            pimpl->positions.emplace_back(lx, proj_y, 0.0F);
        }

        pimpl->dirty = true;
    }

    bool label_renderer::needs_upload() const
    {
        return pimpl->dirty;
    }

    void label_renderer::copy(sdl::copy_pass& pass, sdl::device& dev)
    {
        if(!pimpl->dirty)
        {
            return;
        }

        constexpr float SCALE = 800.0F;
        constexpr float OUTLINE_OFFSET = 2.0F;

        // Build outline geometry
        pimpl->outline_vertices.clear();
        pimpl->outline_indices.clear();
        for(size_t i = 0; i < pimpl->outline_texts.size(); i++)
        {
            glm::vec3 opos = pimpl->positions[i];
            opos.x -= OUTLINE_OFFSET;
            opos.y += OUTLINE_OFFSET;
            pimpl->outline_texts[i].append_geometry(
                pimpl->outline_vertices, pimpl->outline_indices, opos,
                SCALE, 0, 0, 0, 255);
        }

        // Build fill geometry
        pimpl->fill_vertices.clear();
        pimpl->fill_indices.clear();
        for(size_t i = 0; i < pimpl->fill_texts.size(); i++)
        {
            pimpl->fill_texts[i].append_geometry(
                pimpl->fill_vertices, pimpl->fill_indices, pimpl->positions[i],
                SCALE, 255, 255, 255, 255);
        }

        // Upload outline buffers
        pimpl->outline_vertex_buffer.reset();
        pimpl->outline_index_buffer.reset();
        if(!pimpl->outline_vertices.empty())
        {
            auto vbuf = pass.create_and_upload_buffer(
                dev, sdl::buffer_usage::vertex, pimpl->outline_vertices);
            pimpl->outline_vertex_buffer = std::make_unique<sdl::buffer>(std::move(vbuf));

            auto ibuf = pass.create_and_upload_buffer(
                dev, sdl::buffer_usage::index, pimpl->outline_indices);
            pimpl->outline_index_buffer = std::make_unique<sdl::buffer>(std::move(ibuf));
        }

        // Upload fill buffers
        pimpl->fill_vertex_buffer.reset();
        pimpl->fill_index_buffer.reset();
        if(!pimpl->fill_vertices.empty())
        {
            auto vbuf = pass.create_and_upload_buffer(
                dev, sdl::buffer_usage::vertex, pimpl->fill_vertices);
            pimpl->fill_vertex_buffer = std::make_unique<sdl::buffer>(std::move(vbuf));

            auto ibuf = pass.create_and_upload_buffer(
                dev, sdl::buffer_usage::index, pimpl->fill_indices);
            pimpl->fill_index_buffer = std::make_unique<sdl::buffer>(std::move(ibuf));
        }

        pimpl->dirty = false;
    }

    void label_renderer::render(sdl::render_pass& pass,
                                const render_context& ctx,
                                const sdl::sampler& samp,
                                int viewport_width,
                                int viewport_height) const
    {
        if(ctx.current_pass != render_pass_id::text_labels_0)
        {
            return;
        }

        if(!pimpl->fill_vertex_buffer || !pimpl->fill_index_buffer)
        {
            return;
        }

        // Pixel-space orthographic projection (origin bottom-left, Y-up)
        glm::mat4 proj = glm::orthoLH_ZO(
            0.0F, static_cast<float>(viewport_width),
            0.0F, static_cast<float>(viewport_height),
            -1.0F, 1.0F);

        sdl::uniform_buffer uniforms;
        uniforms.projection_matrix = proj;
        uniforms.y_min = -1e9F;
        uniforms.y_max = 1e9F;

        pass.push_vertex_uniforms(0, &uniforms, sizeof(uniforms));
        pass.push_fragment_uniforms(0, &uniforms, sizeof(uniforms));

        // Draw outline (all outline texts share the same atlas)
        if(pimpl->outline_vertex_buffer && pimpl->outline_index_buffer
           && !pimpl->outline_texts.empty())
        {
            pass.bind_fragment_texture_sampler(0, pimpl->outline_texts[0], samp);
            pass.bind_vertex_buffer(*pimpl->outline_vertex_buffer);
            pass.bind_index_buffer(*pimpl->outline_index_buffer);
            pass.draw_indexed(static_cast<uint32_t>(pimpl->outline_indices.size()));
        }

        // Draw fill (all fill texts share the same atlas)
        if(!pimpl->fill_texts.empty())
        {
            pass.bind_fragment_texture_sampler(0, pimpl->fill_texts[0], samp);
            pass.bind_vertex_buffer(*pimpl->fill_vertex_buffer);
            pass.bind_index_buffer(*pimpl->fill_index_buffer);
            pass.draw_indexed(static_cast<uint32_t>(pimpl->fill_indices.size()));
        }
    }

} // namespace nasrbrowse
