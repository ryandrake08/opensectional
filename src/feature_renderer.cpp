#include "feature_renderer.hpp"
#include "feature_builder.hpp"
#include "geo_types.hpp"
#include "line_renderer.hpp"
#include "map_view.hpp"
#include "render_context.hpp"
#include "ui_overlay.hpp"
#include <cmath>
#include <sdl/buffer.hpp>
#include <sdl/copy_pass.hpp>
#include <sdl/device.hpp>
#include <sdl/render_pass.hpp>
#include <sdl/types.hpp>

namespace nasrbrowse
{
    // Query cache tuning
    constexpr double REQUERY_ZOOM_THRESHOLD = 0.5;
    constexpr double QUERY_BBOX_PADDING = 0.5;

    struct feature_renderer::impl
    {
        sdl::device& dev;

        // Background builder (owns worker thread + database connection)
        feature_builder builder;

        // Current view state
        double half_extent_y;
        double aspect_ratio;
        int viewport_height;

        // Cached query bbox (in lon/lat)
        geo_bbox query_bbox;
        double last_zoom;
        bool has_cached_query;

        // Polyline data received from builder (kept for visibility toggling)
        std::array<polyline_data, layer_sdf_count> poly;
        polyline_data selection_overlay;
        line_renderer sdf_lines;

        // Polygon fill (triangulated concave+holes). Rendered in trianglelist_0
        // pass before line_sdf_0 so outlines and icons sit on top.
        std::vector<sdl::vertex_t2f_c4ub_v3f> fill_vertices;
        std::unique_ptr<sdl::buffer> fill_buffer;
        bool fill_needs_upload = false;

        // Labels from most recent build
        std::vector<label_candidate> current_labels;

        layer_visibility vis;
        std::optional<pick_feature> selection;

        impl(sdl::device& dev, const char* db_path, const chart_style& styles)
            : dev(dev)
            , builder(db_path, styles)
            , half_extent_y(HALF_CIRCUMFERENCE)
            , aspect_ratio(1.0)
            , viewport_height(0)
            , query_bbox{0, 0, 0, 0}
            , last_zoom(-1)
            , has_cached_query(false)
        {
        }

        double zoom_level() const
        {
            return nasrbrowse::zoom_level(half_extent_y, viewport_height);
        }

        bool needs_requery(const geo_bbox& bbox) const
        {
            if(!has_cached_query)
            {
                return true;
            }

            double z = zoom_level();
            if(std::abs(z - last_zoom) > REQUERY_ZOOM_THRESHOLD)
            {
                return true;
            }

            return bbox.lon_min < query_bbox.lon_min || bbox.lon_max > query_bbox.lon_max ||
                   bbox.lat_min < query_bbox.lat_min || bbox.lat_max > query_bbox.lat_max;
        }

        void rebuild_sdf_lines()
        {
            std::vector<std::vector<glm::vec2>> all_polylines;
            std::vector<line_style> all_styles;
            std::vector<circle_data> all_circles;

            for(int i = 0; i < layer_sdf_count; i++)
            {
                if(!vis[i]) continue;
                all_polylines.insert(all_polylines.end(),
                    poly[i].polylines.begin(), poly[i].polylines.end());
                all_styles.insert(all_styles.end(),
                    poly[i].styles.begin(), poly[i].styles.end());
                all_circles.insert(all_circles.end(),
                    poly[i].circles.begin(), poly[i].circles.end());
            }

            // Selection overlay rendered last so it sits on top of every layer.
            all_polylines.insert(all_polylines.end(),
                selection_overlay.polylines.begin(), selection_overlay.polylines.end());
            all_styles.insert(all_styles.end(),
                selection_overlay.styles.begin(), selection_overlay.styles.end());
            all_circles.insert(all_circles.end(),
                selection_overlay.circles.begin(), selection_overlay.circles.end());

            sdf_lines.set_data(std::move(all_polylines), std::move(all_styles),
                               std::move(all_circles));
        }
    };

    feature_renderer::feature_renderer(sdl::device& dev, const char* db_path,
                                       const chart_style& cs)
        : pimpl(new impl(dev, db_path, cs))
    {
    }

    feature_renderer::~feature_renderer() = default;

    void feature_renderer::update(double vx_min, double vy_min,
                                   double vx_max, double vy_max,
                                   double half_extent_y, int viewport_height,
                                   double aspect_ratio)
    {
        pimpl->half_extent_y = half_extent_y;
        pimpl->viewport_height = viewport_height;
        pimpl->aspect_ratio = aspect_ratio;

        geo_bbox view_bbox{mx_to_lon(vx_min), my_to_lat(vy_min),
                            mx_to_lon(vx_max), my_to_lat(vy_max)};

        if(pimpl->needs_requery(view_bbox))
        {
            double lon_pad = (view_bbox.lon_max - view_bbox.lon_min) * QUERY_BBOX_PADDING;
            double lat_pad = (view_bbox.lat_max - view_bbox.lat_min) * QUERY_BBOX_PADDING;

            feature_build_request req;
            req.lon_min = view_bbox.lon_min - lon_pad;
            req.lat_min = view_bbox.lat_min - lat_pad;
            req.lon_max = view_bbox.lon_max + lon_pad;
            req.lat_max = view_bbox.lat_max + lat_pad;
            req.half_extent_y = half_extent_y;
            req.viewport_height = viewport_height;
            req.zoom = pimpl->zoom_level();
            req.altitude = pimpl->vis.altitude;
            req.selection = pimpl->selection;
            pimpl->builder.request(req);

            // Speculatively update cache so we don't re-request next frame
            pimpl->query_bbox = {req.lon_min, req.lat_min, req.lon_max, req.lat_max};
            pimpl->last_zoom = req.zoom;
            pimpl->has_cached_query = true;
        }
    }

    bool feature_renderer::drain()
    {
        auto result = pimpl->builder.drain_result();
        if(result)
        {
            pimpl->poly = std::move(result->poly);
            pimpl->selection_overlay = std::move(result->selection_overlay);
            pimpl->current_labels = std::move(result->labels);
            pimpl->rebuild_sdf_lines();

            // Translate triangulated fills into the GPU vertex format.
            auto& src = result->selection_fill.triangles;
            pimpl->fill_vertices.clear();
            pimpl->fill_vertices.reserve(src.size());
            for(const auto& v : src)
            {
                sdl::vertex_t2f_c4ub_v3f out{};
                out.s = 0; out.t = 0;
                out.r = static_cast<uint8_t>(std::clamp(v.color.r, 0.0F, 1.0F) * 255.0F);
                out.g = static_cast<uint8_t>(std::clamp(v.color.g, 0.0F, 1.0F) * 255.0F);
                out.b = static_cast<uint8_t>(std::clamp(v.color.b, 0.0F, 1.0F) * 255.0F);
                out.a = static_cast<uint8_t>(std::clamp(v.color.a, 0.0F, 1.0F) * 255.0F);
                out.x = v.pos.x; out.y = v.pos.y; out.z = 0;
                pimpl->fill_vertices.push_back(out);
            }
            pimpl->fill_needs_upload = true;
            return true;
        }
        return false;
    }

    const std::vector<label_candidate>& feature_renderer::labels() const
    {
        return pimpl->current_labels;
    }

    bool feature_renderer::needs_upload() const
    {
        return pimpl->sdf_lines.needs_upload() || pimpl->fill_needs_upload;
    }

    void feature_renderer::copy(sdl::copy_pass& pass)
    {
        if(pimpl->sdf_lines.needs_upload())
        {
            pimpl->sdf_lines.copy(pass, pimpl->dev);
        }
        if(pimpl->fill_needs_upload)
        {
            pimpl->fill_buffer.reset();
            if(!pimpl->fill_vertices.empty())
            {
                auto buf = pass.create_and_upload_buffer(pimpl->dev,
                    sdl::buffer_usage::vertex, pimpl->fill_vertices);
                pimpl->fill_buffer = std::make_unique<sdl::buffer>(std::move(buf));
            }
            pimpl->fill_needs_upload = false;
        }
    }

    void feature_renderer::render(sdl::render_pass& pass,
                                   const render_context& ctx,
                                   const glm::mat4& view_matrix) const
    {
        if(ctx.current_pass == render_pass_id::polygon_fill_0)
        {
            if(pimpl->fill_buffer && pimpl->fill_buffer->count() > 0)
            {
                sdl::uniform_buffer uniforms;
                uniforms.projection_matrix = ctx.projection_matrix;
                uniforms.view_matrix = view_matrix;
                pass.push_vertex_uniforms(0, &uniforms, sizeof(uniforms));
                pass.push_fragment_uniforms(0, &uniforms, sizeof(uniforms));
                pass.bind_vertex_buffer(*pimpl->fill_buffer);
                pass.draw(pimpl->fill_buffer->count());
            }
        }
        else if(ctx.current_pass == render_pass_id::line_sdf_0)
        {
            int vw = static_cast<int>(pimpl->aspect_ratio * pimpl->viewport_height);
            pimpl->sdf_lines.render(pass, ctx.projection_matrix, view_matrix,
                                    vw, pimpl->viewport_height);
        }
    }

    void feature_renderer::set_selection(std::optional<pick_feature> sel)
    {
        pimpl->selection = std::move(sel);

        // Clear the overlay immediately so stale primitives don't linger
        // until the new build result arrives.
        pimpl->selection_overlay.clear();
        pimpl->rebuild_sdf_lines();

        // Force the worker to rebuild so the new selection's overlay is
        // generated. Clearing has_cached_query ensures update() resubmits
        // a request on the next frame even if the view hasn't changed.
        pimpl->has_cached_query = false;
    }

    void feature_renderer::set_visibility(const layer_visibility& vis)
    {
        bool line_vis_changed = false;
        for(int i = 0; i < layer_sdf_count; i++)
        {
            if(pimpl->vis[i] != vis[i]) line_vis_changed = true;
        }
        bool altitude_changed = pimpl->vis.altitude != vis.altitude;

        pimpl->vis = vis;

        if(altitude_changed)
        {
            // Altitude filter affects which features are queried, not just
            // which cached polylines are packed. Force a full requery.
            pimpl->has_cached_query = false;
        }
        if(line_vis_changed)
        {
            pimpl->rebuild_sdf_lines();
        }
    }

} // namespace nasrbrowse
