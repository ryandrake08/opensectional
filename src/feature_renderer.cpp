#include "feature_renderer.hpp"
#include "chart_style.hpp"
#include "line_renderer.hpp"
#include "map_view.hpp"
#include "nasr_database.hpp"
#include "render_context.hpp"
#include "ui_overlay.hpp"
#include <cmath>
#include <glm/ext/matrix_transform.hpp>
#include <sdl/buffer.hpp>
#include <sdl/copy_pass.hpp>
#include <sdl/device.hpp>
#include <sdl/render_pass.hpp>
#include <sdl/types.hpp>
#include <vector>

namespace nasrbrowse
{
    // Polyline data for a feature type
    struct polyline_data
    {
        std::vector<std::vector<glm::vec2>> polylines;
        std::vector<line_style> styles;

        void clear()
        {
            polylines.clear();
            styles.clear();
        }
    };

    // Convert feature_style color to uint8 for point features
    static uint8_t to_u8(float v)
    {
        return static_cast<uint8_t>(v * 255.0F + 0.5F);
    }

    // Convert feature_style to line_style for SDF line features
    static line_style to_line_style(const feature_style& fs)
    {
        return {fs.line_width, fs.border_width, fs.dash_length, fs.gap_length,
                fs.r, fs.g, fs.b, fs.a};
    }

    // Extract airway style key from airway ID
    static std::string airway_key(const std::string& id)
    {
        if(id.size() >= 3 && id[0] == 'R' && id[1] == 'T' && id[2] == 'E')
            return "airway_rte";
        if(id.size() >= 2)
        {
            if(id[0] == 'B' && id[1] == 'R') return "airway_br";
            if(id[0] == 'T' && id[1] == 'K') return "airway_tk";
            if(id[0] == 'A' && id[1] == 'R') return "airway_ar";
        }
        if(!id.empty())
        {
            char c = id[0];
            std::string key = "airway_";
            key += static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
            return key;
        }
        return "airway_v";
    }

    // Map SUA type code to style key
    static const char* sua_key(const std::string& sua_type)
    {
        if(sua_type == "RA") return "sua_restricted";
        if(sua_type == "PA") return "sua_prohibited";
        if(sua_type == "WA") return "sua_warning";
        if(sua_type == "AA") return "sua_alert";
        if(sua_type == "NSA") return "sua_nsa";
        return "sua_moa";
    }

    // Map class airspace code to style key
    static const char* airspace_key(const std::string& cls)
    {
        if(cls == "B") return "airspace_b";
        if(cls == "C") return "airspace_c";
        if(cls == "D") return "airspace_d";
        return "airspace_e";
    }

    // Airport zoom key based on runway classification
    static const char* airport_zoom_key(const airport& apt)
    {
        if(apt.hard_surface == "HARD" && apt.max_rwy_len > 8069)
            return "airport_hard_long";
        if(apt.hard_surface == "HARD")
            return "airport_hard_short";
        return "airport_other";
    }

    // Airport color key based on site/tower type
    static const char* airport_color_key(const airport& apt)
    {
        if(apt.site_type_code == "H") return "airport_heliport";
        if(apt.twr_type_code != "NON-ATCT") return "airport_towered";
        return "airport_untowered";
    }

    // Obstacle style key based on AGL height
    static const char* obstacle_key(int agl_ht)
    {
        if(agl_ht >= 1000) return "obstacle_1000ft";
        if(agl_ht >= 200) return "obstacle_200ft";
        return "obstacle_low";
    }

    struct feature_renderer::impl
    {
        sdl::device& dev;
        nasr_database db;
        chart_style styles;

        // Current view state
        double center_x, center_y, half_extent_y;
        double aspect_ratio;
        int viewport_height;

        // Cached query bbox (in lon/lat)
        double query_lon_min, query_lat_min, query_lon_max, query_lat_max;
        double last_zoom;
        bool has_cached_query;

        // Point feature vertex buffers (rendered as line_list)
        std::vector<sdl::vertex_t2f_c4ub_v3f> airport_vertices;
        std::vector<sdl::vertex_t2f_c4ub_v3f> navaid_vertices;
        std::vector<sdl::vertex_t2f_c4ub_v3f> fix_vertices;
        std::vector<sdl::vertex_t2f_c4ub_v3f> obstacle_vertices;

        std::unique_ptr<sdl::buffer> airport_buffer;
        std::unique_ptr<sdl::buffer> navaid_buffer;
        std::unique_ptr<sdl::buffer> fix_buffer;
        std::unique_ptr<sdl::buffer> obstacle_buffer;

        // Line/polygon feature polylines (rendered via SDF line_renderer)
        polyline_data sua_poly;
        polyline_data airspace_poly;
        polyline_data airway_poly;
        polyline_data runway_poly;
        line_renderer sdf_lines;

        bool vis_airports = true;
        bool vis_runways = true;
        bool vis_navaids = true;
        bool vis_fixes = true;
        bool vis_airways = true;
        bool vis_airspace = true;
        bool vis_sua = true;
        bool vis_obstacles = true;
        bool dirty;

        impl(sdl::device& dev, const char* db_path, const chart_style& styles)
            : dev(dev)
            , db(db_path)
            , styles(styles)
            , center_x(0)
            , center_y(0)
            , half_extent_y(HALF_CIRCUMFERENCE)
            , aspect_ratio(1.0)
            , viewport_height(0)
            , query_lon_min(0)
            , query_lat_min(0)
            , query_lon_max(0)
            , query_lat_max(0)
            , last_zoom(-1)
            , has_cached_query(false)
            , dirty(false)
        {
        }

        double zoom_level() const
        {
            double meters_per_pixel = (half_extent_y * 2.0) / viewport_height;
            double world_size = 2.0 * HALF_CIRCUMFERENCE;
            return std::log2(world_size / (256.0 * meters_per_pixel));
        }

        bool needs_requery(double lon_min, double lat_min,
                           double lon_max, double lat_max) const
        {
            if(!has_cached_query)
            {
                return true;
            }

            double z = zoom_level();
            if(std::abs(z - last_zoom) > 0.5)
            {
                return true;
            }

            return lon_min < query_lon_min || lon_max > query_lon_max ||
                   lat_min < query_lat_min || lat_max > query_lat_max;
        }

        void build_vertices(double lon_min, double lat_min,
                             double lon_max, double lat_max)
        {
            double z = zoom_level();

            double lon_pad = (lon_max - lon_min) * 0.5;
            double lat_pad = (lat_max - lat_min) * 0.5;
            double qlon_min = lon_min - lon_pad;
            double qlat_min = lat_min - lat_pad;
            double qlon_max = lon_max + lon_pad;
            double qlat_max = lat_max + lat_pad;

            query_lon_min = qlon_min;
            query_lat_min = qlat_min;
            query_lon_max = qlon_max;
            query_lat_max = qlat_max;
            last_zoom = z;
            has_cached_query = true;

            airport_vertices.clear();
            navaid_vertices.clear();
            fix_vertices.clear();
            obstacle_vertices.clear();
            sua_poly.clear();
            airspace_poly.clear();
            airway_poly.clear();
            runway_poly.clear();

            build_airport_vertices(qlon_min, qlat_min, qlon_max, qlat_max, z);
            build_navaid_vertices(qlon_min, qlat_min, qlon_max, qlat_max, z);
            build_airway_polylines(qlon_min, qlat_min, qlon_max, qlat_max, z);
            build_sua_polylines(qlon_min, qlat_min, qlon_max, qlat_max, z);
            build_obstacle_vertices(qlon_min, qlat_min, qlon_max, qlat_max, z);
            build_fix_vertices(qlon_min, qlat_min, qlon_max, qlat_max, z);
            build_runway_polylines(qlon_min, qlat_min, qlon_max, qlat_max, z);
            build_airspace_polylines(qlon_min, qlat_min, qlon_max, qlat_max, z);

            rebuild_sdf_lines();
            dirty = true;
        }

        void add_diamond(std::vector<sdl::vertex_t2f_c4ub_v3f>& verts,
                         double mx, double my, float radius,
                         uint8_t r, uint8_t g, uint8_t b, uint8_t a)
        {
            float x = static_cast<float>(mx);
            float y = static_cast<float>(my);
            verts.push_back({0, 0, r, g, b, a, x - radius, y, 0});
            verts.push_back({0, 0, r, g, b, a, x, y + radius, 0});
            verts.push_back({0, 0, r, g, b, a, x, y + radius, 0});
            verts.push_back({0, 0, r, g, b, a, x + radius, y, 0});
            verts.push_back({0, 0, r, g, b, a, x + radius, y, 0});
            verts.push_back({0, 0, r, g, b, a, x, y - radius, 0});
            verts.push_back({0, 0, r, g, b, a, x, y - radius, 0});
            verts.push_back({0, 0, r, g, b, a, x - radius, y, 0});
        }

        void add_triangle(std::vector<sdl::vertex_t2f_c4ub_v3f>& verts,
                          double mx, double my, float radius,
                          uint8_t r, uint8_t g, uint8_t b, uint8_t a)
        {
            float x = static_cast<float>(mx);
            float y = static_cast<float>(my);
            float h = radius * 0.866F;
            verts.push_back({0, 0, r, g, b, a, x, y + radius, 0});
            verts.push_back({0, 0, r, g, b, a, x + h, y - radius * 0.5F, 0});
            verts.push_back({0, 0, r, g, b, a, x + h, y - radius * 0.5F, 0});
            verts.push_back({0, 0, r, g, b, a, x - h, y - radius * 0.5F, 0});
            verts.push_back({0, 0, r, g, b, a, x - h, y - radius * 0.5F, 0});
            verts.push_back({0, 0, r, g, b, a, x, y + radius, 0});
        }

        void build_airport_vertices(double lon_min, double lat_min,
                                     double lon_max, double lat_max, double z)
        {
            const auto& airports = db.query_airports(lon_min, lat_min, lon_max, lat_max);
            float radius = static_cast<float>(half_extent_y * 0.008);

            for(const auto& apt : airports)
            {
                if(!styles.visible(airport_zoom_key(apt), z))
                {
                    continue;
                }

                const auto& cs = styles.get(airport_color_key(apt));
                double mx = lon_to_mx(apt.lon);
                double my = lat_to_my(apt.lat);
                add_diamond(airport_vertices, mx, my, radius,
                            to_u8(cs.r), to_u8(cs.g), to_u8(cs.b), to_u8(cs.a));
            }
        }

        void build_navaid_vertices(double lon_min, double lat_min,
                                    double lon_max, double lat_max, double z)
        {
            const auto& navaids = db.query_navaids(lon_min, lat_min, lon_max, lat_max);
            float radius = static_cast<float>(half_extent_y * 0.006);

            for(const auto& nav : navaids)
            {
                const char* key = (nav.nav_type == "NDB" || nav.nav_type == "NDB/DME")
                    ? "navaid_ndb" : "navaid_vor";

                if(!styles.visible(key, z))
                {
                    continue;
                }

                const auto& cs = styles.get(key);
                double mx = lon_to_mx(nav.lon);
                double my = lat_to_my(nav.lat);
                add_diamond(navaid_vertices, mx, my, radius,
                            to_u8(cs.r), to_u8(cs.g), to_u8(cs.b), to_u8(cs.a));
            }
        }

        void build_fix_vertices(double lon_min, double lat_min,
                                 double lon_max, double lat_max, double z)
        {
            if(!styles.visible("fix", z))
            {
                return;
            }

            const auto& fixes = db.query_fixes(lon_min, lat_min, lon_max, lat_max);
            float radius = static_cast<float>(half_extent_y * 0.003);
            const auto& cs = styles.get("fix");

            for(const auto& fix : fixes)
            {
                double mx = lon_to_mx(fix.lon);
                double my = lat_to_my(fix.lat);
                add_triangle(fix_vertices, mx, my, radius,
                             to_u8(cs.r), to_u8(cs.g), to_u8(cs.b), to_u8(cs.a));
            }
        }

        void build_airway_polylines(double lon_min, double lat_min,
                                     double lon_max, double lat_max, double z)
        {
            const auto& airways = db.query_airways(lon_min, lat_min, lon_max, lat_max);

            for(const auto& seg : airways)
            {
                std::string key = airway_key(seg.awy_id);

                if(!styles.visible(key, z))
                {
                    continue;
                }

                const auto& fs = styles.get(key);

                float x0 = static_cast<float>(lon_to_mx(seg.from_lon));
                float y0 = static_cast<float>(lat_to_my(seg.from_lat));
                float x1 = static_cast<float>(lon_to_mx(seg.to_lon));
                float y1 = static_cast<float>(lat_to_my(seg.to_lat));

                airway_poly.polylines.push_back({glm::vec2(x0, y0), glm::vec2(x1, y1)});
                airway_poly.styles.push_back(to_line_style(fs));
            }
        }

        void build_runway_polylines(double lon_min, double lat_min,
                                     double lon_max, double lat_max, double z)
        {
            if(!styles.visible("runway", z))
            {
                return;
            }

            const auto& runways = db.query_runways(lon_min, lat_min, lon_max, lat_max);
            const auto& fs = styles.get("runway");

            for(const auto& rwy : runways)
            {
                float x0 = static_cast<float>(lon_to_mx(rwy.end1_lon));
                float y0 = static_cast<float>(lat_to_my(rwy.end1_lat));
                float x1 = static_cast<float>(lon_to_mx(rwy.end2_lon));
                float y1 = static_cast<float>(lat_to_my(rwy.end2_lat));

                runway_poly.polylines.push_back({glm::vec2(x0, y0), glm::vec2(x1, y1)});
                runway_poly.styles.push_back(to_line_style(fs));
            }
        }

        void build_sua_polylines(double lon_min, double lat_min,
                                  double lon_max, double lat_max, double z)
        {
            const auto& suas = db.query_sua(lon_min, lat_min, lon_max, lat_max);

            for(const auto& s : suas)
            {
                if(s.shape.size() < 2)
                {
                    continue;
                }

                const char* key = sua_key(s.sua_type);

                if(!styles.visible(key, z))
                {
                    continue;
                }

                const auto& fs = styles.get(key);

                std::vector<glm::vec2> polyline;
                polyline.reserve(s.shape.size() + 1);
                for(const auto& pt : s.shape)
                {
                    polyline.emplace_back(
                        static_cast<float>(lon_to_mx(pt.lon)),
                        static_cast<float>(lat_to_my(pt.lat)));
                }
                if(polyline.size() > 2)
                {
                    polyline.push_back(polyline.front());
                }

                sua_poly.polylines.push_back(std::move(polyline));
                sua_poly.styles.push_back(to_line_style(fs));
            }
        }

        void build_obstacle_vertices(double lon_min, double lat_min,
                                      double lon_max, double lat_max, double z)
        {
            const auto& obstacles = db.query_obstacles(lon_min, lat_min, lon_max, lat_max);
            float radius = static_cast<float>(half_extent_y * 0.003);

            for(const auto& obs : obstacles)
            {
                const char* key = obstacle_key(obs.agl_ht);

                if(!styles.visible(key, z))
                {
                    continue;
                }

                const auto& cs = styles.get(key);
                double mx = lon_to_mx(obs.lon);
                double my = lat_to_my(obs.lat);
                add_triangle(obstacle_vertices, mx, my, -radius,
                             to_u8(cs.r), to_u8(cs.g), to_u8(cs.b), to_u8(cs.a));
            }
        }

        void build_airspace_polylines(double lon_min, double lat_min,
                                       double lon_max, double lat_max, double z)
        {
            const auto& airspaces = db.query_class_airspace(lon_min, lat_min, lon_max, lat_max);

            for(const auto& arsp : airspaces)
            {
                const char* key = airspace_key(arsp.airspace_class);

                if(!styles.visible(key, z))
                {
                    continue;
                }

                const auto& fs = styles.get(key);

                for(const auto& ring : arsp.parts)
                {
                    if(ring.size() < 2)
                    {
                        continue;
                    }

                    std::vector<glm::vec2> polyline;
                    polyline.reserve(ring.size() + 1);
                    for(const auto& pt : ring)
                    {
                        polyline.emplace_back(
                            static_cast<float>(lon_to_mx(pt.lon)),
                            static_cast<float>(lat_to_my(pt.lat)));
                    }
                    if(polyline.size() > 2 && polyline.front() != polyline.back())
                    {
                        polyline.push_back(polyline.front());
                    }

                    airspace_poly.polylines.push_back(std::move(polyline));
                    airspace_poly.styles.push_back(to_line_style(fs));
                }
            }
        }

        void rebuild_sdf_lines()
        {
            std::vector<std::vector<glm::vec2>> all_polylines;
            std::vector<line_style> all_styles;

            auto append = [&](const polyline_data& pd)
            {
                all_polylines.insert(all_polylines.end(),
                    pd.polylines.begin(), pd.polylines.end());
                all_styles.insert(all_styles.end(),
                    pd.styles.begin(), pd.styles.end());
            };

            if(vis_sua) append(sua_poly);
            if(vis_airspace) append(airspace_poly);
            if(vis_airways) append(airway_poly);
            if(vis_runways) append(runway_poly);

            sdf_lines.set_polylines(std::move(all_polylines), std::move(all_styles));
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
                                   int viewport_height, double aspect_ratio)
    {
        pimpl->center_x = (vx_min + vx_max) * 0.5;
        pimpl->center_y = (vy_min + vy_max) * 0.5;
        pimpl->half_extent_y = (vy_max - vy_min) * 0.5;
        pimpl->viewport_height = viewport_height;
        pimpl->aspect_ratio = aspect_ratio;

        double lon_min = mx_to_lon(vx_min);
        double lon_max = mx_to_lon(vx_max);
        double lat_min = my_to_lat(vy_min);
        double lat_max = my_to_lat(vy_max);

        if(pimpl->needs_requery(lon_min, lat_min, lon_max, lat_max))
        {
            pimpl->build_vertices(lon_min, lat_min, lon_max, lat_max);
        }
    }

    bool feature_renderer::needs_upload()
    {
        return pimpl->dirty || pimpl->sdf_lines.needs_upload();
    }

    void feature_renderer::copy(sdl::copy_pass& pass)
    {
        if(pimpl->dirty)
        {
            auto upload = [&](auto& verts, auto& buffer)
            {
                buffer.reset();
                if(!verts.empty())
                {
                    auto buf = pass.create_and_upload_buffer(
                        pimpl->dev, sdl::buffer_usage::vertex, verts);
                    buffer = std::make_unique<sdl::buffer>(std::move(buf));
                }
            };

            upload(pimpl->obstacle_vertices, pimpl->obstacle_buffer);
            upload(pimpl->fix_vertices, pimpl->fix_buffer);
            upload(pimpl->navaid_vertices, pimpl->navaid_buffer);
            upload(pimpl->airport_vertices, pimpl->airport_buffer);
            pimpl->dirty = false;
        }

        if(pimpl->sdf_lines.needs_upload())
        {
            pimpl->sdf_lines.copy(pass, pimpl->dev);
        }
    }

    void feature_renderer::render(sdl::render_pass& pass,
                                   const render_context& ctx) const
    {
        float s = static_cast<float>(1.0 / (2.0 * pimpl->half_extent_y));
        float cx = static_cast<float>(pimpl->center_x);
        float cy = static_cast<float>(pimpl->center_y);

        glm::mat4 view_matrix = glm::scale(glm::mat4(1.0F), glm::vec3(s, s, 1.0F)) *
                                 glm::translate(glm::mat4(1.0F), glm::vec3(-cx, -cy, 0.0F));

        if(ctx.current_pass == render_pass_id::trianglelist_0)
        {
            sdl::uniform_buffer uniforms;
            uniforms.projection_matrix = ctx.projection_matrix;
            uniforms.view_matrix = view_matrix;

            auto draw = [&](const auto& buffer)
            {
                if(buffer && buffer->count() > 0)
                {
                    pass.push_vertex_uniforms(0, &uniforms, sizeof(uniforms));
                    pass.push_fragment_uniforms(0, &uniforms, sizeof(uniforms));
                    pass.bind_vertex_buffer(*buffer);
                    pass.draw(buffer->count());
                }
            };

            if(pimpl->vis_obstacles) draw(pimpl->obstacle_buffer);
            if(pimpl->vis_fixes) draw(pimpl->fix_buffer);
            if(pimpl->vis_navaids) draw(pimpl->navaid_buffer);
            if(pimpl->vis_airports) draw(pimpl->airport_buffer);
        }
        else if(ctx.current_pass == render_pass_id::line_sdf_0)
        {
            int vw = static_cast<int>(pimpl->aspect_ratio * pimpl->viewport_height);
            pimpl->sdf_lines.render(pass, ctx.projection_matrix, view_matrix,
                                    vw, pimpl->viewport_height);
        }
    }

    void feature_renderer::set_visibility(const layer_visibility& vis)
    {
        bool line_vis_changed =
            pimpl->vis_runways != vis.runways ||
            pimpl->vis_airways != vis.airways ||
            pimpl->vis_airspace != vis.airspace ||
            pimpl->vis_sua != vis.sua;

        pimpl->vis_airports = vis.airports;
        pimpl->vis_runways = vis.runways;
        pimpl->vis_navaids = vis.navaids;
        pimpl->vis_fixes = vis.fixes;
        pimpl->vis_airways = vis.airways;
        pimpl->vis_airspace = vis.airspace;
        pimpl->vis_sua = vis.sua;
        pimpl->vis_obstacles = vis.obstacles;

        if(line_vis_changed)
        {
            pimpl->rebuild_sdf_lines();
        }
    }

} // namespace nasrbrowse
