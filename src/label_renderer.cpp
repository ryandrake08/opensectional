#include "label_renderer.hpp"
#include "render_context.hpp"
#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
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
    // Label layout constants
    constexpr auto LABEL_HEIGHT = 16.0F;
    constexpr auto LABEL_OFFSET_Y = 24.0F;
    constexpr auto LABEL_PAD_X = 4.0F;
    constexpr auto LABEL_PAD_Y = 2.0F;

    // Fill color (white) and outline color (black)
    constexpr uint8_t FILL_R = 255, FILL_G = 255, FILL_B = 255, FILL_A = 255;
    constexpr uint8_t OUTLINE_R = 0, OUTLINE_G = 0, OUTLINE_B = 0, OUTLINE_A = 255;

    // A label with its cached text objects and world-space position
    struct cached_label
    {
        sdl::text fill;
        sdl::text outline;
        double mx, my;
        float width;
        float height;
        float angle;
        int priority;
        int layer;
        uint8_t outline_r = OUTLINE_R;
        uint8_t outline_g = OUTLINE_G;
        uint8_t outline_b = OUTLINE_B;

        // Composite airspace label: TYPE on left, upper/lower altitudes
        // on right separated by a divider line.
        std::optional<sdl::text> upper_fill = std::nullopt, upper_outline = std::nullopt;
        std::optional<sdl::text> lower_fill = std::nullopt, lower_outline = std::nullopt;
        std::optional<sdl::text> divider_fill = std::nullopt, divider_outline = std::nullopt;
        float type_width = 0.0F;
        float upper_width = 0.0F;
        float lower_width = 0.0F;
        float divider_width = 0.0F;
        float right_col_width = 0.0F;
        float line_height = 0.0F;
    };

    struct label_renderer::impl
    {
        sdl::device& dev;
        sdl::text_engine& engine;
        sdl::font& font;
        sdl::font& outline_font;
        // Cached label text objects (rebuilt only on set_candidates)
        std::vector<cached_label> labels;

        // Indices of visible labels after overlap elimination (rebuilt each frame)
        std::vector<size_t> visible;

        // Screen positions for visible labels (Y-up, for projection)
        std::vector<glm::vec3> positions;

        // GPU buffers
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
        : pimpl(std::make_unique<impl>(dev, engine, font, outline_font))
    {
    }

    label_renderer::~label_renderer() = default;

    void label_renderer::set_candidates(const std::vector<label_candidate>& candidates)
    {
        pimpl->labels.clear();
        pimpl->labels.reserve(candidates.size());

        for(const auto& lc : candidates)
        {
            auto fill = sdl::text(pimpl->engine, pimpl->font, lc.text.c_str());
            auto bounds = fill.get_bounds();

            if(lc.upper_text.empty())
            {
                pimpl->labels.push_back(cached_label{
                    .fill = std::move(fill),
                    .outline = sdl::text(pimpl->engine, pimpl->outline_font,
                                         lc.text.c_str()),
                    .mx = lc.mx,
                    .my = lc.my,
                    .width = bounds.width(),
                    .height = bounds.height(),
                    .angle = lc.angle,
                    .priority = lc.priority,
                    .layer = lc.layer,
                });
                continue;
            }

            // Composite airspace label
            constexpr auto COMPOSITE_GAP = 3.0F;

            auto uf = sdl::text(pimpl->engine, pimpl->font, lc.upper_text.c_str());
            auto lf = sdl::text(pimpl->engine, pimpl->font, lc.lower_text.c_str());
            auto upper_w = uf.get_bounds().width();
            auto lower_w = lf.get_bounds().width();
            auto right_w = std::max(upper_w, lower_w);

            std::string dashes(static_cast<size_t>(right_w / 4.0F + 1.5F), '-');
            auto df = sdl::text(pimpl->engine, pimpl->font, dashes.c_str());
            auto divider_w = df.get_bounds().width();
            right_w = std::max(right_w, divider_w);

            auto type_w = bounds.width();
            auto total_w = type_w + COMPOSITE_GAP + right_w;
            auto line_h = bounds.height();
            auto total_h = line_h * 3.0F;

            pimpl->labels.push_back(cached_label{
                .fill = std::move(fill),
                .outline = sdl::text(pimpl->engine, pimpl->outline_font,
                                     lc.text.c_str()),
                .mx = lc.mx,
                .my = lc.my,
                .width = total_w,
                .height = total_h,
                .angle = 0.0F,
                .priority = lc.priority,
                .layer = lc.layer,
                .outline_r = lc.outline_r,
                .outline_g = lc.outline_g,
                .outline_b = lc.outline_b,
                .upper_fill = std::move(uf),
                .upper_outline = sdl::text(pimpl->engine, pimpl->outline_font,
                                            lc.upper_text.c_str()),
                .lower_fill = std::move(lf),
                .lower_outline = sdl::text(pimpl->engine, pimpl->outline_font,
                                            lc.lower_text.c_str()),
                .divider_fill = std::move(df),
                .divider_outline = sdl::text(pimpl->engine, pimpl->outline_font,
                                              dashes.c_str()),
                .type_width = type_w,
                .upper_width = upper_w,
                .lower_width = lower_w,
                .divider_width = divider_w,
                .right_col_width = right_w,
                .line_height = line_h,
            });
        }
    }

    void label_renderer::update_positions(double center_x, double center_y,
                                          double half_extent_y,
                                          int viewport_width, int viewport_height,
                                          const layer_visibility& vis)
    {
        // World-to-screen transform
        auto scale = viewport_height / (2.0 * half_extent_y);
        auto screen_cx = viewport_width * 0.5;
        auto screen_cy = viewport_height * 0.5;

        // Build sorted index by priority (descending)
        std::vector<size_t> sorted_indices;
        sorted_indices.reserve(pimpl->labels.size());
        for(size_t i = 0; i < pimpl->labels.size(); i++)
        {
            sorted_indices.push_back(i);
        }
        std::sort(sorted_indices.begin(), sorted_indices.end(),
            [this](size_t a, size_t b)
            {
                return pimpl->labels[a].priority > pimpl->labels[b].priority;
            });

        // Greedy overlap elimination
        struct placed_rect { float x0, y0, x1, y1; };
        std::vector<placed_rect> placed;
        placed.reserve(sorted_indices.size());

        pimpl->visible.clear();
        pimpl->positions.clear();

        for(size_t idx : sorted_indices)
        {
            const auto& lbl = pimpl->labels[idx];

            if(!vis[lbl.layer])
            {
                continue;
            }

            // Project world to screen (Y-down)
            auto sx = static_cast<float>(
                (lbl.mx - center_x) * scale + screen_cx);
            auto sy = static_cast<float>(
                screen_cy - (lbl.my - center_y) * scale);

            float x0;
            float y0;
            float x1;
            float y1;
            glm::vec3 pos;

            if(lbl.upper_fill)
            {
                // Composite airspace label: centered at screen point
                x0 = sx - (lbl.width * 0.5F + LABEL_PAD_X);
                y0 = sy - (lbl.height * 0.5F + LABEL_PAD_Y);
                x1 = sx + (lbl.width * 0.5F + LABEL_PAD_X);
                y1 = sy + (lbl.height * 0.5F + LABEL_PAD_Y);

                auto proj_y = static_cast<float>(viewport_height) - sy;
                pos = {sx, proj_y, 0.0F};
            }
            else if(lbl.angle == 0.0F)
            {
                // Point label: centered horizontally, offset above symbol
                auto lx = sx - lbl.width * 0.5F;
                auto ly = sy - LABEL_OFFSET_Y - LABEL_HEIGHT;

                x0 = lx - LABEL_PAD_X;
                y0 = ly - LABEL_PAD_Y;
                x1 = lx + lbl.width + LABEL_PAD_X;
                y1 = ly + LABEL_HEIGHT + LABEL_PAD_Y;

                auto proj_y = static_cast<float>(viewport_height) - (sy - LABEL_OFFSET_Y);
                pos = {lx, proj_y, 0.0F};
            }
            else
            {
                // Line label: centered on the line, rotated
                auto abs_cos = std::abs(std::cos(lbl.angle));
                auto abs_sin = std::abs(std::sin(lbl.angle));
                auto aabb_w = lbl.width * abs_cos + lbl.height * abs_sin;
                auto aabb_h = lbl.width * abs_sin + lbl.height * abs_cos;

                x0 = sx - (aabb_w * 0.5F + LABEL_PAD_X);
                y0 = sy - (aabb_h * 0.5F + LABEL_PAD_Y);
                x1 = sx + (aabb_w * 0.5F + LABEL_PAD_X);
                y1 = sy + (aabb_h * 0.5F + LABEL_PAD_Y);

                auto proj_y = static_cast<float>(viewport_height) - sy;
                pos = {sx, proj_y, 0.0F};
            }

            // Skip if off-screen
            if(x1 < 0 || x0 > viewport_width || y1 < 0 || y0 > viewport_height)
            {
                continue;
            }

            // Check overlap with already-placed labels
            auto overlaps = false;
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

            pimpl->visible.push_back(idx);
            pimpl->positions.push_back(pos);
        }

        // Rebuild vertex/index geometry from cached text objects
        pimpl->outline_vertices.clear();
        pimpl->outline_indices.clear();
        pimpl->fill_vertices.clear();
        pimpl->fill_indices.clear();

        constexpr auto COMPOSITE_GAP = 3.0F;
        auto ofs = static_cast<float>(pimpl->outline_font.get_outline() * 2);

        for(size_t i = 0; i < pimpl->visible.size(); i++)
        {
            auto idx = pimpl->visible[i];
            const auto& pos = pimpl->positions[i];
            auto& lbl = pimpl->labels[idx];

            if(lbl.upper_fill)
            {
                // Composite airspace label. pos is center (Y-up proj).
                // Layout:
                //        UPPER
                //  TYPE ------
                //        LOWER
                auto left_x = pos.x - lbl.width * 0.5F;
                auto right_x = left_x + lbl.type_width + COMPOSITE_GAP;
                auto mid_y = pos.y;

                // TYPE: right-justified, vertically centered
                glm::vec3 type_pos(right_x - lbl.type_width - COMPOSITE_GAP,
                                   mid_y - lbl.line_height * 0.5F, 0.0F);
                glm::vec3 type_opos(type_pos.x - ofs, type_pos.y + ofs, 0.0F);
                lbl.outline.append_geometry(
                    pimpl->outline_vertices, pimpl->outline_indices, type_opos,
                    lbl.outline_r, lbl.outline_g, lbl.outline_b, OUTLINE_A);
                lbl.fill.append_geometry(
                    pimpl->fill_vertices, pimpl->fill_indices, type_pos,
                    FILL_R, FILL_G, FILL_B, FILL_A);

                // Center of right column for centering text
                auto right_cx = right_x + lbl.right_col_width * 0.5F;

                // UPPER: above divider, centered on right column
                auto upper_x = right_cx - lbl.upper_width * 0.5F;
                glm::vec3 upper_pos(upper_x, mid_y + lbl.line_height * 0.5F, 0.0F);
                glm::vec3 upper_opos(upper_pos.x - ofs, upper_pos.y + ofs, 0.0F);
                lbl.upper_outline->append_geometry(
                    pimpl->outline_vertices, pimpl->outline_indices, upper_opos,
                    lbl.outline_r, lbl.outline_g, lbl.outline_b, OUTLINE_A);
                lbl.upper_fill->append_geometry(
                    pimpl->fill_vertices, pimpl->fill_indices, upper_pos,
                    FILL_R, FILL_G, FILL_B, FILL_A);

                // DIVIDER: centered on right column
                auto div_x = right_cx - lbl.divider_width * 0.5F;
                glm::vec3 div_pos(div_x, mid_y - lbl.line_height * 0.5F, 0.0F);
                glm::vec3 div_opos(div_pos.x - ofs, div_pos.y + ofs, 0.0F);
                lbl.divider_outline->append_geometry(
                    pimpl->outline_vertices, pimpl->outline_indices, div_opos,
                    lbl.outline_r, lbl.outline_g, lbl.outline_b, OUTLINE_A);
                lbl.divider_fill->append_geometry(
                    pimpl->fill_vertices, pimpl->fill_indices, div_pos,
                    FILL_R, FILL_G, FILL_B, FILL_A);

                // LOWER: below divider, centered on right column
                auto lower_x = right_cx - lbl.lower_width * 0.5F;
                glm::vec3 lower_pos(lower_x,
                                    mid_y - lbl.line_height * 1.5F, 0.0F);
                glm::vec3 lower_opos(lower_pos.x - ofs, lower_pos.y + ofs, 0.0F);
                lbl.lower_outline->append_geometry(
                    pimpl->outline_vertices, pimpl->outline_indices, lower_opos,
                    lbl.outline_r, lbl.outline_g, lbl.outline_b, OUTLINE_A);
                lbl.lower_fill->append_geometry(
                    pimpl->fill_vertices, pimpl->fill_indices, lower_pos,
                    FILL_R, FILL_G, FILL_B, FILL_A);
            }
            else if(lbl.angle == 0.0F)
            {
                glm::vec3 opos(pos.x - ofs, pos.y + ofs, 0.0F);
                lbl.outline.append_geometry(
                    pimpl->outline_vertices, pimpl->outline_indices, opos,
                    OUTLINE_R, OUTLINE_G, OUTLINE_B, OUTLINE_A);

                lbl.fill.append_geometry(
                    pimpl->fill_vertices, pimpl->fill_indices, pos,
                    FILL_R, FILL_G, FILL_B, FILL_A);
            }
            else
            {
                lbl.outline.append_geometry(
                    pimpl->outline_vertices, pimpl->outline_indices,
                    pos, lbl.angle,
                    lbl.outline_r, lbl.outline_g, lbl.outline_b, OUTLINE_A);

                lbl.fill.append_geometry(
                    pimpl->fill_vertices, pimpl->fill_indices,
                    pos, lbl.angle,
                    FILL_R, FILL_G, FILL_B, FILL_A);
            }
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

        if(!pimpl->fill_vertex_buffer || !pimpl->fill_index_buffer || pimpl->visible.empty())
        {
            return;
        }

        // Pixel-space orthographic projection (origin bottom-left, Y-up)
        auto proj = glm::orthoLH_ZO(
            0.0F, static_cast<float>(viewport_width),
            0.0F, static_cast<float>(viewport_height),
            -1.0F, 1.0F);

        auto uniforms = sdl::uniform_buffer{};
        uniforms.projection_matrix = proj;
        uniforms.y_min = -1e9F;
        uniforms.y_max = 1e9F;

        pass.push_vertex_uniforms(0, &uniforms, sizeof(uniforms));
        pass.push_fragment_uniforms(0, &uniforms, sizeof(uniforms));

        // Draw outline (all outline texts share the same atlas)
        if(pimpl->outline_vertex_buffer && pimpl->outline_index_buffer)
        {
            auto first_visible = pimpl->visible[0];
            pass.bind_fragment_texture_sampler(0, pimpl->labels[first_visible].outline, samp);
            pass.bind_vertex_buffer(*pimpl->outline_vertex_buffer);
            pass.bind_index_buffer(*pimpl->outline_index_buffer);
            pass.draw_indexed(static_cast<uint32_t>(pimpl->outline_indices.size()));
        }

        // Draw fill (all fill texts share the same atlas)
        {
            auto first_visible = pimpl->visible[0];
            pass.bind_fragment_texture_sampler(0, pimpl->labels[first_visible].fill, samp);
            pass.bind_vertex_buffer(*pimpl->fill_vertex_buffer);
            pass.bind_index_buffer(*pimpl->fill_index_buffer);
            pass.draw_indexed(static_cast<uint32_t>(pimpl->fill_indices.size()));
        }
    }

} // namespace nasrbrowse
