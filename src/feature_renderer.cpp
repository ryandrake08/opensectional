#include "feature_renderer.hpp"
#include "feature_builder.hpp"
#include "line_renderer.hpp"
#include "map_view.hpp"
#include "render_context.hpp"
#include "ui_overlay.hpp"
#include <cmath>
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
        double query_lon_min, query_lat_min, query_lon_max, query_lat_max;
        double last_zoom;
        bool has_cached_query;

        // Polyline data received from builder (kept for visibility toggling)
        std::array<polyline_data, layer_sdf_count> poly;
        line_renderer sdf_lines;

        // Labels from most recent build
        std::vector<label_candidate> current_labels;

        layer_visibility vis;

        impl(sdl::device& dev, const char* db_path, const chart_style& styles)
            : dev(dev)
            , builder(db_path, styles)
            , half_extent_y(HALF_CIRCUMFERENCE)
            , aspect_ratio(1.0)
            , viewport_height(0)
            , query_lon_min(0)
            , query_lat_min(0)
            , query_lon_max(0)
            , query_lat_max(0)
            , last_zoom(-1)
            , has_cached_query(false)
        {
        }

        double zoom_level() const
        {
            return nasrbrowse::zoom_level(half_extent_y, viewport_height);
        }

        bool needs_requery(double lon_min, double lat_min,
                           double lon_max, double lat_max) const
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

            return lon_min < query_lon_min || lon_max > query_lon_max ||
                   lat_min < query_lat_min || lat_max > query_lat_max;
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

        double lon_min = mx_to_lon(vx_min);
        double lon_max = mx_to_lon(vx_max);
        double lat_min = my_to_lat(vy_min);
        double lat_max = my_to_lat(vy_max);

        if(pimpl->needs_requery(lon_min, lat_min, lon_max, lat_max))
        {
            double lon_pad = (lon_max - lon_min) * QUERY_BBOX_PADDING;
            double lat_pad = (lat_max - lat_min) * QUERY_BBOX_PADDING;

            feature_build_request req;
            req.lon_min = lon_min - lon_pad;
            req.lat_min = lat_min - lat_pad;
            req.lon_max = lon_max + lon_pad;
            req.lat_max = lat_max + lat_pad;
            req.half_extent_y = half_extent_y;
            req.viewport_height = viewport_height;
            req.zoom = pimpl->zoom_level();
            pimpl->builder.request(req);

            // Speculatively update cache so we don't re-request next frame
            pimpl->query_lon_min = req.lon_min;
            pimpl->query_lat_min = req.lat_min;
            pimpl->query_lon_max = req.lon_max;
            pimpl->query_lat_max = req.lat_max;
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
            pimpl->current_labels = std::move(result->labels);
            pimpl->rebuild_sdf_lines();
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
        return pimpl->sdf_lines.needs_upload();
    }

    void feature_renderer::copy(sdl::copy_pass& pass)
    {
        if(pimpl->sdf_lines.needs_upload())
        {
            pimpl->sdf_lines.copy(pass, pimpl->dev);
        }
    }

    void feature_renderer::render(sdl::render_pass& pass,
                                   const render_context& ctx,
                                   const glm::mat4& view_matrix) const
    {
        if(ctx.current_pass == render_pass_id::line_sdf_0)
        {
            int vw = static_cast<int>(pimpl->aspect_ratio * pimpl->viewport_height);
            pimpl->sdf_lines.render(pass, ctx.projection_matrix, view_matrix,
                                    vw, pimpl->viewport_height);
        }
    }

    void feature_renderer::set_visibility(const layer_visibility& vis)
    {
        bool line_vis_changed = false;
        for(int i = 0; i < layer_sdf_count; i++)
        {
            if(pimpl->vis[i] != vis[i]) line_vis_changed = true;
        }

        pimpl->vis = vis;

        if(line_vis_changed)
        {
            pimpl->rebuild_sdf_lines();
        }
    }

} // namespace nasrbrowse
