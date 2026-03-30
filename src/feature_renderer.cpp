#include "feature_renderer.hpp"
#include "chart_style.hpp"
#include "line_renderer.hpp"
#include "map_view.hpp"
#include "nasr_database.hpp"
#include "render_context.hpp"
#include "ui_overlay.hpp"
#include <cmath>
#include <cstring>
#include <unordered_set>
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
                fs.r, fs.g, fs.b, fs.a, 0};
    }

    // Symbol base radii as fraction of view half-extent
    constexpr double SYMBOL_RADIUS_AIRPORT  = 0.012;
    constexpr double SYMBOL_RADIUS_FIX      = 0.012;
    constexpr double SYMBOL_RADIUS_OBSTACLE = 0.003;

    // Vector letter sizing (shared by airport and PJA icons)
    constexpr float LETTER_HEIGHT    = 0.385F;  // height relative to symbol radius
    constexpr float LETTER_ASPECT    = 0.7F;    // width/height ratio
    constexpr float LETTER_WIDTH_PX  = 2.0F;    // stroke width in pixels

    // Interior fill for outlined+filled symbols (airports, PJA diamonds)
    constexpr float SYMBOL_FILL_PX   = 50.0F;   // fill_width in pixels, large enough to fill any symbol

    // Query cache tuning
    constexpr double REQUERY_ZOOM_THRESHOLD = 0.5;
    constexpr double QUERY_BBOX_PADDING = 0.5;

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
        std::vector<sdl::vertex_t2f_c4ub_v3f> obstacle_vertices;
        std::unique_ptr<sdl::buffer> obstacle_buffer;

        // Navaid positions for airway clearance (populated during build_navaid_polylines)
        std::vector<glm::vec2> navaid_positions;
        float navaid_clearance;

        // Waypoint names on visible airways (populated during build_airway_polylines)
        std::unordered_set<std::string> airway_waypoints;

        // Line/polygon feature polylines (rendered via SDF line_renderer)
        std::array<polyline_data, layer_sdf_count> poly;
        line_renderer sdf_lines;

        layer_visibility vis;
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

        void build_vertices(double lon_min, double lat_min,
                             double lon_max, double lat_max)
        {
            double z = zoom_level();

            double lon_pad = (lon_max - lon_min) * QUERY_BBOX_PADDING;
            double lat_pad = (lat_max - lat_min) * QUERY_BBOX_PADDING;
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

            for(auto& p : poly) p.clear();
            obstacle_vertices.clear();

            build_airport_polylines(qlon_min, qlat_min, qlon_max, qlat_max, z);
            build_navaid_polylines(qlon_min, qlat_min, qlon_max, qlat_max, z);
            build_airway_polylines(qlon_min, qlat_min, qlon_max, qlat_max, z);
            build_mtr_polylines(qlon_min, qlat_min, qlon_max, qlat_max, z);
            build_sua_polylines(qlon_min, qlat_min, qlon_max, qlat_max, z);
            build_pja_polylines(qlon_min, qlat_min, qlon_max, qlat_max, z);
            build_maa_polylines(qlon_min, qlat_min, qlon_max, qlat_max, z);
            build_adiz_polylines(qlon_min, qlat_min, qlon_max, qlat_max, z);
            build_artcc_polylines(qlon_min, qlat_min, qlon_max, qlat_max, z);
            build_obstacle_vertices(qlon_min, qlat_min, qlon_max, qlat_max, z);
            build_fix_polylines(qlon_min, qlat_min, qlon_max, qlat_max, z);
            build_runway_polylines(qlon_min, qlat_min, qlon_max, qlat_max, z);
            build_airspace_polylines(qlon_min, qlat_min, qlon_max, qlat_max, z);

            rebuild_sdf_lines();
            dirty = true;
        }

        void add_triangle(std::vector<sdl::vertex_t2f_c4ub_v3f>& verts,
                          double mx, double my, float radius,
                          uint8_t r, uint8_t g, uint8_t b, uint8_t a)
        {
            float x = static_cast<float>(mx);
            float y = static_cast<float>(my);
            constexpr float SQRT3_2 = 0.866F;
            float h = radius * SQRT3_2;
            verts.push_back({0, 0, r, g, b, a, x, y + radius, 0});
            verts.push_back({0, 0, r, g, b, a, x + h, y - radius * 0.5F, 0});
            verts.push_back({0, 0, r, g, b, a, x + h, y - radius * 0.5F, 0});
            verts.push_back({0, 0, r, g, b, a, x - h, y - radius * 0.5F, 0});
            verts.push_back({0, 0, r, g, b, a, x - h, y - radius * 0.5F, 0});
            verts.push_back({0, 0, r, g, b, a, x, y + radius, 0});
        }

        // Add a circle polyline to a polyline_data
        void add_circle_to(polyline_data& pd, float cx, float cy,
                           float r, const line_style& ls, int n = 24)
        {
            std::vector<glm::vec2> pts;
            for(int i = 0; i < n; i++)
            {
                float angle = 2.0F * static_cast<float>(M_PI) * i / n;
                pts.emplace_back(cx + r * std::cos(angle), cy + r * std::sin(angle));
            }
            pts.push_back(pts.front());
            pd.polylines.push_back(std::move(pts));
            pd.styles.push_back(ls);
        }

        // Add a line segment to a polyline_data
        void add_seg_to(polyline_data& pd, float x0, float y0,
                        float x1, float y1, const line_style& ls)
        {
            pd.polylines.push_back({glm::vec2(x0, y0), glm::vec2(x1, y1)});
            pd.styles.push_back(ls);
        }

        // Vector letter segment data: pairs of (x,y) endpoints in normalized coords.
        // x is scaled by w (half-width), y is scaled by h (half-height) and
        // offset from center. Midline is at y=0.1.
        struct letter_def
        {
            const float* segs;
            int count;  // number of segments (pairs of xy pairs)
        };

        // clang-format off
        static constexpr float segs_H[] = {-1,-1, -1,1,  1,-1, 1,1,  -1,.1f, 1,.1f};
        static constexpr float segs_F[] = {-1,-1, -1,1,  -1,1, 1,1,  -1,.1f, .6f,.1f};
        static constexpr float segs_G[] = {1,1, -1,1,  -1,1, -1,-1,  -1,-1, 1,-1,  1,-1, 1,.1f,  1,.1f, 0,.1f};
        static constexpr float segs_S[] = {1,1, -1,1,  -1,1, -1,.1f,  -1,.1f, 1,.1f,  1,.1f, 1,-1,  1,-1, -1,-1};
        static constexpr float segs_B[] = {-1,-1, -1,1,  -1,1, 1,1,  1,1, 1,.1f,  1,.1f, -1,.1f,  -1,-1, 1,-1,  1,-1, 1,.1f};
        static constexpr float segs_X[] = {-1,-1, 1,1,  -1,1, 1,-1};
        static constexpr float segs_M[] = {-1,-1, -1,1,  -1,1, 0,.1f,  0,.1f, 1,1,  1,1, 1,-1};
        static constexpr float segs_R[] = {-1,-1, -1,1,  -1,1, 1,1,  1,1, 1,.1f,  1,.1f, -1,.1f,  0,.1f, 1,-1};
        static constexpr float segs_P[] = {-1,-1, -1,1,  -1,1, 1,1,  1,1, 1,.1f,  1,.1f, -1,.1f};
        static constexpr float segs_A[] = {-1,-1, 0,1,  0,1, 1,-1,  -0.5f,.1f, 0.5f,.1f};
        static constexpr float segs_U[] = {-1,1, -1,-1,  -1,-1, 1,-1,  1,-1, 1,1};
        // clang-format on

        static constexpr letter_def letter_H = {segs_H, 3};
        static constexpr letter_def letter_F = {segs_F, 3};
        static constexpr letter_def letter_G = {segs_G, 5};
        static constexpr letter_def letter_S = {segs_S, 5};
        static constexpr letter_def letter_B = {segs_B, 6};
        static constexpr letter_def letter_X = {segs_X, 2};
        static constexpr letter_def letter_M = {segs_M, 4};
        static constexpr letter_def letter_R = {segs_R, 5};
        static constexpr letter_def letter_P = {segs_P, 4};
        static constexpr letter_def letter_A = {segs_A, 3};
        static constexpr letter_def letter_U = {segs_U, 3};

        void draw_letter(polyline_data& pd, const letter_def& ld,
                         float cx, float cy, float w, float h,
                         const line_style& ls)
        {
            for(int i = 0; i < ld.count; i++)
            {
                float x0 = cx + ld.segs[i * 4 + 0] * w;
                float y0 = cy + ld.segs[i * 4 + 1] * h;
                float x1 = cx + ld.segs[i * 4 + 2] * w;
                float y1 = cy + ld.segs[i * 4 + 3] * h;
                add_seg_to(pd, x0, y0, x1, y1, ls);
            }
        }

        bool is_military(const airport& apt)
        {
            return apt.ownership_type_code == "MA" ||
                   apt.ownership_type_code == "MR" ||
                   apt.ownership_type_code == "MN" ||
                   apt.ownership_type_code == "CG";
        }

        bool is_towered(const airport& apt)
        {
            return apt.twr_type_code.substr(0, 4) == "ATCT";
        }

        void build_airport_polylines(double lon_min, double lat_min,
                                      double lon_max, double lat_max, double z)
        {
            const auto& airports = db.query_airports(lon_min, lat_min, lon_max, lat_max);
            // Airport symbol geometry sizing
            constexpr float APT_OUTER_SCALE = 1.2F;     // outer radius relative to base
            constexpr float APT_RING_WIDTH_PX = 1.0F;    // ring stroke width in pixels
            constexpr float APT_FILL_RADIUS = 0.5F;      // filled circle radius relative to symbol_r

            float r = static_cast<float>(half_extent_y * SYMBOL_RADIUS_AIRPORT);
            float pixels_per_world = static_cast<float>(viewport_height / (2.0 * half_extent_y));

            for(const auto& apt : airports)
            {
                if(!styles.airport_visible(apt, z))
                {
                    continue;
                }

                // Select polyline_data based on site type
                auto& pd = [&]() -> polyline_data&
                {
                    if(apt.site_type_code == "H") return poly[layer_heliports];
                    if(apt.site_type_code == "C") return poly[layer_seaplane];
                    if(apt.site_type_code == "U") return poly[layer_ultralight];
                    if(apt.site_type_code == "G") return poly[layer_gliderports];
                    if(apt.site_type_code == "B") return poly[layer_balloonports];
                    return poly[layer_airports];
                }();

                const auto& cs = styles.airport_style(apt);
                float cx = static_cast<float>(lon_to_mx(apt.lon));
                float cy = static_cast<float>(lat_to_my(apt.lat));

                float symbol_r = r * APT_OUTER_SCALE;
                float ring_geom_r = symbol_r - (APT_RING_WIDTH_PX * 0.5F) / pixels_per_world;
                line_style ring_ls = {APT_RING_WIDTH_PX, 1.0F, 0, 0, cs.r, cs.g, cs.b, cs.a, 0};

                bool closed = apt.arpt_status == "CI" || apt.arpt_status == "CP";
                bool pvt = apt.facility_use_code == "PR";
                bool mil = is_military(apt);
                float h = ring_geom_r * LETTER_HEIGHT;
                line_style white_ls = {LETTER_WIDTH_PX, 0, 0, 0, 1.0F, 1.0F, 1.0F, 1.0F, 0};
                float w = h * LETTER_ASPECT;

                const letter_def* letter = nullptr;
                if(closed)                          letter = &letter_X;
                else if(apt.site_type_code == "H")  letter = &letter_H;
                else if(apt.site_type_code == "C")  letter = &letter_S;
                else if(apt.site_type_code == "U")  letter = &letter_F;
                else if(apt.site_type_code == "G")  letter = &letter_G;
                else if(apt.site_type_code == "B")  letter = &letter_B;
                else if(mil)                        letter = &letter_M;
                else if(pvt)                        letter = &letter_R;

                if(apt.hard_surface)
                {
                    float geom_r = symbol_r * APT_FILL_RADIUS;
                    float fill_px = symbol_r * pixels_per_world;
                    line_style fill_ls = {fill_px, 1.0F, 0, 0, cs.r, cs.g, cs.b, cs.a, 0};
                    add_circle_to(pd, cx, cy, geom_r, fill_ls);
                    if(letter) draw_letter(pd, *letter, cx, cy, w, h, white_ls);
                }
                else if(letter)
                {
                    line_style filled_ring = ring_ls;
                    filled_ring.fill_width = SYMBOL_FILL_PX;
                    add_circle_to(pd, cx, cy, ring_geom_r, filled_ring);
                    draw_letter(pd, *letter, cx, cy, w, h, white_ls);
                }
                else
                {
                    // Soft surface public: hollow ring
                    add_circle_to(pd, cx, cy, ring_geom_r, ring_ls);
                }
            }
        }

        // --- Navaid symbol geometry builders ---

        // Closed hexagon with flat top/bottom, corners on left/right
        void add_hexagon(float cx, float cy, float r, const line_style& ls)
        {
            std::vector<glm::vec2> pts;
            for(int i = 0; i < 6; i++)
            {
                float angle = glm::radians(60.0F * i);
                pts.emplace_back(cx + r * std::cos(angle), cy + r * std::sin(angle));
            }
            pts.push_back(pts.front());
            poly[layer_navaids].polylines.push_back(std::move(pts));
            poly[layer_navaids].styles.push_back(ls);
        }

        // Small dot (diamond) at center
        void add_center_dot(float cx, float cy, float r, const line_style& ls)
        {
            float d = r * 0.15F;
            poly[layer_navaids].polylines.push_back({
                {cx - d, cy}, {cx, cy + d}, {cx + d, cy}, {cx, cy - d}, {cx - d, cy}
            });
            poly[layer_navaids].styles.push_back(ls);
        }

        // Axis-aligned rectangle
        void add_rect(float cx, float cy, float hw, float hh, const line_style& ls)
        {
            poly[layer_navaids].polylines.push_back({
                {cx - hw, cy - hh}, {cx + hw, cy - hh},
                {cx + hw, cy + hh}, {cx - hw, cy + hh}, {cx - hw, cy - hh}
            });
            poly[layer_navaids].styles.push_back(ls);
        }

        // Three rectangles sharing edges with the hexagon at 1:30, 10:30, 6:00
        void add_caltrop(float cx, float cy, float hex_r, const line_style& ls)
        {
            // Hexagon vertices at 0°, 60°, 120°, 180°, 240°, 300°
            float vx[6], vy[6];
            for(int i = 0; i < 6; i++)
            {
                float angle = glm::radians(60.0F * i);
                vx[i] = cx + hex_r * std::cos(angle);
                vy[i] = cy + hex_r * std::sin(angle);
            }

            // Each rectangle shares one hexagon edge (inner side) and extends outward.
            // Edges: V0→V1 (1:30), V2→V3 (10:30), V4→V5 (6:00)
            // Outward normal bisects the edge angle: 30°, 150°, 270°
            int edges[][2] = {{0, 1}, {2, 3}, {4, 5}};
            float normal_angles[] = {30.0F, 150.0F, 270.0F};
            float h = hex_r * 0.5F;

            for(int e = 0; e < 3; e++)
            {
                int a = edges[e][0], b = edges[e][1];
                float na = glm::radians(normal_angles[e]);
                float nx = std::cos(na) * h;
                float ny = std::sin(na) * h;

                poly[layer_navaids].polylines.push_back({
                    {vx[a], vy[a]},
                    {vx[b], vy[b]},
                    {vx[b] + nx, vy[b] + ny},
                    {vx[a] + nx, vy[a] + ny},
                    {vx[a], vy[a]},
                });
                poly[layer_navaids].styles.push_back(ls);
            }
        }

        // Circle approximated as a 16-sided polygon
        void add_circle(float cx, float cy, float r, const line_style& ls)
        {
            const int n = 16;
            std::vector<glm::vec2> pts;
            for(int i = 0; i < n; i++)
            {
                float angle = 2.0F * static_cast<float>(M_PI) * i / n;
                pts.emplace_back(cx + r * std::cos(angle), cy + r * std::sin(angle));
            }
            pts.push_back(pts.front());
            poly[layer_navaids].polylines.push_back(std::move(pts));
            poly[layer_navaids].styles.push_back(ls);
        }

        void build_navaid_polylines(double lon_min, double lat_min,
                                     double lon_max, double lat_max, double z)
        {
            // Navaid symbol sizing (relative to base radius)
            constexpr float NAV_NDB_CIRCLE = 0.4F;
            constexpr float NAV_DME_RECT = 0.85F;
            constexpr float NAV_VORDME_WIDTH = 1.1F;
            constexpr float NAV_CLEARANCE = 2.0F;

            const auto& navaids = db.query_navaids(lon_min, lat_min, lon_max, lat_max);
            float r = static_cast<float>(half_extent_y * SYMBOL_RADIUS_AIRPORT);

            navaid_positions.clear();
            navaid_clearance = r * NAV_CLEARANCE;

            for(const auto& nav : navaids)
            {
                // Skip types we don't depict
                if(nav.nav_type == "VOT" || nav.nav_type == "FAN MARKER" ||
                   nav.nav_type == "MARINE NDB")
                {
                    continue;
                }

                if(!styles.navaid_visible(nav.nav_type, z))
                {
                    continue;
                }

                const auto& fs = styles.navaid_style(nav.nav_type);
                line_style ls = to_line_style(fs);
                line_style filled_ls = ls;
                filled_ls.fill_width = SYMBOL_FILL_PX;

                float cx = static_cast<float>(lon_to_mx(nav.lon));
                float cy = static_cast<float>(lat_to_my(nav.lat));

                navaid_positions.emplace_back(cx, cy);

                if(nav.nav_type == "NDB")
                {
                    add_circle(cx, cy, r * NAV_NDB_CIRCLE, filled_ls);
                }
                else if(nav.nav_type == "NDB/DME")
                {
                    add_circle(cx, cy, r * NAV_NDB_CIRCLE, filled_ls);
                    add_rect(cx, cy, r * NAV_DME_RECT, r * NAV_DME_RECT, ls);
                }
                else if(nav.nav_type == "DME")
                {
                    add_rect(cx, cy, r * NAV_DME_RECT, r * NAV_DME_RECT, filled_ls);
                }
                else if(std::strcmp(nav.nav_type.c_str(), "VOR/DME") == 0)
                {
                    add_hexagon(cx, cy, r, filled_ls);
                    add_center_dot(cx, cy, r, ls);
                    add_rect(cx, cy, r * NAV_VORDME_WIDTH, r * NAV_DME_RECT, ls);
                }
                else if(nav.nav_type == "VORTAC" || nav.nav_type == "TACAN")
                {
                    add_hexagon(cx, cy, r, filled_ls);
                    add_center_dot(cx, cy, r, ls);
                    add_caltrop(cx, cy, r, ls);
                }
                else
                {
                    // VOR (plain)
                    add_hexagon(cx, cy, r, filled_ls);
                    add_center_dot(cx, cy, r, ls);
                }
            }
        }

        // Add a triangle as an SDF polyline
        void add_triangle_polyline(polyline_data& pd,
                                    float cx, float cy, float r, const line_style& ls)
        {
            constexpr float SQRT3_2 = 0.866F;
            float h = r * SQRT3_2;
            pd.polylines.push_back({
                {cx, cy + r},
                {cx - h, cy - r * 0.5F},
                {cx + h, cy - r * 0.5F},
                {cx, cy + r},
            });
            pd.styles.push_back(ls);
        }

        // Add a four-pointed waypoint star with concave arc edges and inner circle
        void add_waypoint_star_polyline(polyline_data& pd,
                                         float cx, float cy, float r, const line_style& ls)
        {
            constexpr int ARC_SEGS = 3;
            constexpr float PI = 3.14159265F;
            constexpr float SQRT2_M1 = 0.41421356F;  // sqrt(2) - 1

            // Star points at cardinal directions: right, up, left, down
            // Between adjacent points, draw a concave arc centered at the diagonal
            // Arc centers: (R,R), (-R,R), (-R,-R), (R,-R) relative to symbol center
            // Each arc subtends 90 degrees
            struct arc_def { float acx, acy; float start_angle; };
            const arc_def arcs[4] = {
                { r,  r, -PI / 2},   // right→up,   center at (+R,+R), start -90°
                {-r,  r,  0},        // up→left,     center at (-R,+R), start 0°
                {-r, -r,  PI / 2},   // left→down,   center at (-R,-R), start 90°
                { r, -r,  PI},       // down→right,  center at (+R,-R), start 180°
            };

            std::vector<glm::vec2> star_pts;
            for(int q = 0; q < 4; q++)
            {
                float acx_off = arcs[q].acx;
                float acy_off = arcs[q].acy;
                float a0 = arcs[q].start_angle;
                for(int i = 0; i <= ARC_SEGS; i++)
                {
                    float t = static_cast<float>(i) / ARC_SEGS;
                    float angle = a0 - t * (PI / 2);
                    float px = cx + acx_off + r * std::cos(angle);
                    float py = cy + acy_off + r * std::sin(angle);
                    star_pts.push_back({px, py});
                }
            }
            star_pts.push_back(star_pts[0]);
            pd.polylines.push_back(std::move(star_pts));
            pd.styles.push_back(ls);

            // Inner circle at radius where arcs reach their midpoint
            constexpr int CIRCLE_SEGS = 8;
            float cr = r * SQRT2_M1;
            std::vector<glm::vec2> circle_pts;
            for(int i = 0; i <= CIRCLE_SEGS; i++)
            {
                float angle = 2 * PI * static_cast<float>(i) / CIRCLE_SEGS;
                circle_pts.push_back({cx + cr * std::cos(angle), cy + cr * std::sin(angle)});
            }
            pd.polylines.push_back(std::move(circle_pts));
            pd.styles.push_back(ls);
        }

        bool fix_on_airway(const std::string& fix_id) const
        {
            return airway_waypoints.find(fix_id) != airway_waypoints.end();
        }

        void build_fix_polylines(double lon_min, double lat_min,
                                  double lon_max, double lat_max, double z)
        {
            const auto& fixes = db.query_fixes(lon_min, lat_min, lon_max, lat_max);
            float radius = static_cast<float>(half_extent_y * SYMBOL_RADIUS_FIX);

            for(const auto& fix : fixes)
            {
                if(!styles.fix_visible(fix_on_airway(fix.fix_id), z))
                {
                    continue;
                }

                const auto& fs = styles.fix_style(fix.use_code);
                line_style ls = to_line_style(fs);

                float cx = static_cast<float>(lon_to_mx(fix.lon));
                float cy = static_cast<float>(lat_to_my(fix.lat));

                line_style filled_ls = ls;
                filled_ls.fill_width = SYMBOL_FILL_PX;

                if(fix.use_code == "RP" || fix.use_code == "MR")
                {
                    add_triangle_polyline(poly[layer_fixes], cx, cy, radius, filled_ls);
                }
                else
                {
                    add_waypoint_star_polyline(poly[layer_fixes], cx, cy, radius, filled_ls);
                }
            }
        }

        // Check if a point is at a navaid position (within tight tolerance)
        bool is_at_navaid(float x, float y) const
        {
            constexpr float NAVAID_OVERLAP_TOL = 0.1F;
            float tol = navaid_clearance * NAVAID_OVERLAP_TOL;
            float tol_sq = tol * tol;
            for(const auto& np : navaid_positions)
            {
                float dx = x - np.x;
                float dy = y - np.y;
                if(dx * dx + dy * dy < tol_sq)
                {
                    return true;
                }
            }
            return false;
        }

        void build_airway_polylines(double lon_min, double lat_min,
                                     double lon_max, double lat_max, double z)
        {
            airway_waypoints.clear();
            const auto& airways = db.query_airways(lon_min, lat_min, lon_max, lat_max);

            for(const auto& seg : airways)
            {
                if(!styles.airway_visible(seg.awy_id, z))
                {
                    continue;
                }

                airway_waypoints.insert(seg.from_point);
                airway_waypoints.insert(seg.to_point);

                const auto& fs = styles.airway_style(seg.awy_id);

                float x0 = static_cast<float>(lon_to_mx(seg.from_lon));
                float y0 = static_cast<float>(lat_to_my(seg.from_lat));
                float x1 = static_cast<float>(lon_to_mx(seg.to_lon));
                float y1 = static_cast<float>(lat_to_my(seg.to_lat));

                // Shorten segment ends that coincide with navaids
                float dx = x1 - x0;
                float dy = y1 - y0;
                float len = std::sqrt(dx * dx + dy * dy);
                if(len > 0.0F)
                {
                    float ux = dx / len;
                    float uy = dy / len;

                    if(is_at_navaid(x0, y0))
                    {
                        x0 += ux * navaid_clearance;
                        y0 += uy * navaid_clearance;
                    }
                    if(is_at_navaid(x1, y1))
                    {
                        x1 -= ux * navaid_clearance;
                        y1 -= uy * navaid_clearance;
                    }
                }

                // Skip if shortening collapsed the segment
                if((x1 - x0) * dx + (y1 - y0) * dy <= 0.0F)
                {
                    continue;
                }

                poly[layer_airways].polylines.push_back({glm::vec2(x0, y0), glm::vec2(x1, y1)});
                poly[layer_airways].styles.push_back(to_line_style(fs));
            }
        }

        void build_mtr_polylines(double lon_min, double lat_min,
                                  double lon_max, double lat_max, double z)
        {
            if(!styles.mtr_visible(z))
            {
                return;
            }

            const auto& mtrs = db.query_mtrs(lon_min, lat_min, lon_max, lat_max);
            const auto& fs = styles.mtr_style();

            for(const auto& seg : mtrs)
            {
                float x0 = static_cast<float>(lon_to_mx(seg.from_lon));
                float y0 = static_cast<float>(lat_to_my(seg.from_lat));
                float x1 = static_cast<float>(lon_to_mx(seg.to_lon));
                float y1 = static_cast<float>(lat_to_my(seg.to_lat));

                poly[layer_mtrs].polylines.push_back({glm::vec2(x0, y0), glm::vec2(x1, y1)});
                poly[layer_mtrs].styles.push_back(to_line_style(fs));
            }
        }

        void build_runway_polylines(double lon_min, double lat_min,
                                     double lon_max, double lat_max, double z)
        {
            if(!styles.runway_visible(z))
            {
                return;
            }

            const auto& runways = db.query_runways(lon_min, lat_min, lon_max, lat_max);
            const auto& fs = styles.runway_style();

            for(const auto& rwy : runways)
            {
                float x0 = static_cast<float>(lon_to_mx(rwy.end1_lon));
                float y0 = static_cast<float>(lat_to_my(rwy.end1_lat));
                float x1 = static_cast<float>(lon_to_mx(rwy.end2_lon));
                float y1 = static_cast<float>(lat_to_my(rwy.end2_lat));

                poly[layer_runways].polylines.push_back({glm::vec2(x0, y0), glm::vec2(x1, y1)});
                poly[layer_runways].styles.push_back(to_line_style(fs));
            }
        }

        // Convert a ring of airspace_points to world coords, close it, and append to output
        void append_polygon_ring(polyline_data& output,
                                 const std::vector<airspace_point>& points,
                                 const line_style& ls)
        {
            if(points.size() < 2)
            {
                return;
            }

            std::vector<glm::vec2> polyline;
            polyline.reserve(points.size() + 1);
            for(const auto& pt : points)
            {
                polyline.emplace_back(
                    static_cast<float>(lon_to_mx(pt.lon)),
                    static_cast<float>(lat_to_my(pt.lat)));
            }
            if(polyline.size() > 2 && polyline.front() != polyline.back())
            {
                polyline.push_back(polyline.front());
            }

            output.polylines.push_back(std::move(polyline));
            output.styles.push_back(ls);
        }

        void build_sua_polylines(double lon_min, double lat_min,
                                  double lon_max, double lat_max, double z)
        {
            const auto& suas = db.query_sua(lon_min, lat_min, lon_max, lat_max);

            for(const auto& s : suas)
            {
                if(!styles.sua_visible(s.sua_type, z)) continue;
                auto ls = to_line_style(styles.sua_style(s.sua_type));

                for(const auto& ring : s.parts)
                {
                    append_polygon_ring(poly[layer_sua], ring.points, ls);
                }
            }
        }

        void build_pja_polylines(double lon_min, double lat_min,
                                  double lon_max, double lat_max, double z)
        {
            bool area_vis = styles.pja_area_visible(z);
            bool point_vis = styles.pja_point_visible(z);
            if(!area_vis && !point_vis) return;

            constexpr int CIRCLE_SEGS = 24;
            constexpr double PI = 3.14159265358979;
            constexpr double NM_TO_DEG_LAT = 1.0 / 60.0;
            constexpr double SYMBOL_RADIUS_PJA = SYMBOL_RADIUS_AIRPORT;

            const auto& pjas = db.query_pjas(lon_min, lat_min, lon_max, lat_max);

            for(const auto& p : pjas)
            {
                if(p.radius_nm > 0.0 && area_vis)
                {
                    // Generate circle polygon in geographic coordinates
                    double dlat = p.radius_nm * NM_TO_DEG_LAT;
                    double dlon = dlat / std::cos(p.lat * PI / 180.0);

                    std::vector<airspace_point> circle;
                    for(int i = 0; i <= CIRCLE_SEGS; i++)
                    {
                        double angle = 2.0 * PI * i / CIRCLE_SEGS;
                        circle.push_back({
                            p.lat + dlat * std::sin(angle),
                            p.lon + dlon * std::cos(angle)});
                    }

                    auto ls = to_line_style(styles.pja_area_style());
                    append_polygon_ring(poly[layer_pja], circle, ls);
                }
                else if(p.radius_nm <= 0.0 && point_vis)
                {
                    float cx = static_cast<float>(lon_to_mx(p.lon));
                    float cy = static_cast<float>(lat_to_my(p.lat));
                    float r = static_cast<float>(half_extent_y * SYMBOL_RADIUS_PJA);

                    // Diamond with tan outline and black filled interior
                    // using fill_width to extend the inside border
                    auto ls = to_line_style(styles.pja_point_style());
                    ls.fill_width = SYMBOL_FILL_PX;
                    poly[layer_pja].polylines.push_back({
                        {cx + r, cy}, {cx, cy + r}, {cx - r, cy},
                        {cx, cy - r}, {cx + r, cy},
                    });
                    poly[layer_pja].styles.push_back(ls);

                    // White "P" letter
                    float lh = r * LETTER_HEIGHT;
                    float lw = lh * LETTER_ASPECT;
                    line_style white_ls = {LETTER_WIDTH_PX, 0, 0, 0, 1, 1, 1, 1, 0};
                    draw_letter(poly[layer_pja], letter_P, cx, cy, lw, lh, white_ls);
                }
            }
        }

        static const letter_def* maa_letter(const std::string& type)
        {
            if(type == "AEROBATIC PRACTICE") return &letter_A;
            if(type == "GLIDER")             return &letter_G;
            if(type == "HANG GLIDER")        return &letter_H;
            if(type == "ULTRALIGHT")         return &letter_U;
            if(type == "SPACE LAUNCH")       return &letter_S;
            return nullptr;
        }

        void build_maa_polylines(double lon_min, double lat_min,
                                  double lon_max, double lat_max, double z)
        {
            bool area_vis = styles.maa_area_visible(z);
            bool point_vis = styles.maa_point_visible(z);
            if(!area_vis && !point_vis) return;

            constexpr int CIRCLE_SEGS = 24;
            constexpr double PI = 3.14159265358979;
            constexpr double NM_TO_DEG_LAT = 1.0 / 60.0;

            const auto& maas = db.query_maas(lon_min, lat_min, lon_max, lat_max);

            for(const auto& m : maas)
            {
                if(!m.shape.empty() && area_vis)
                {
                    // Shape-defined: render as dashed polygon
                    auto ls = to_line_style(styles.maa_area_style());
                    append_polygon_ring(poly[layer_maa], m.shape, ls);
                }
                else if(m.radius_nm > 0.0 && area_vis)
                {
                    // Point+radius: render as dashed circle
                    double dlat = m.radius_nm * NM_TO_DEG_LAT;
                    double dlon = dlat / std::cos(m.lat * PI / 180.0);

                    std::vector<airspace_point> circle;
                    for(int i = 0; i <= CIRCLE_SEGS; i++)
                    {
                        double angle = 2.0 * PI * i / CIRCLE_SEGS;
                        circle.push_back({
                            m.lat + dlat * std::sin(angle),
                            m.lon + dlon * std::cos(angle)});
                    }

                    auto ls = to_line_style(styles.maa_area_style());
                    append_polygon_ring(poly[layer_maa], circle, ls);
                }
                else if(m.lat != 0.0 && point_vis)
                {
                    // Point-only: diamond with filled interior + letter
                    float cx = static_cast<float>(lon_to_mx(m.lon));
                    float cy = static_cast<float>(lat_to_my(m.lat));
                    float r = static_cast<float>(half_extent_y * SYMBOL_RADIUS_AIRPORT);

                    auto ls = to_line_style(styles.maa_point_style());
                    ls.fill_width = SYMBOL_FILL_PX;
                    poly[layer_maa].polylines.push_back({
                        {cx + r, cy}, {cx, cy + r}, {cx - r, cy},
                        {cx, cy - r}, {cx + r, cy},
                    });
                    poly[layer_maa].styles.push_back(ls);

                    const letter_def* ld = maa_letter(m.type);
                    if(ld)
                    {
                        float lh = r * LETTER_HEIGHT;
                        float lw = lh * LETTER_ASPECT;
                        line_style white_ls = {LETTER_WIDTH_PX, 0, 0, 0, 1, 1, 1, 1, 0};
                        draw_letter(poly[layer_maa], *ld, cx, cy, lw, lh, white_ls);
                    }
                }
            }
        }

        void build_adiz_polylines(double lon_min, double lat_min,
                                   double lon_max, double lat_max, double z)
        {
            if(!styles.adiz_visible(z)) return;

            const auto& adizs = db.query_adiz(lon_min, lat_min, lon_max, lat_max);
            auto ls = to_line_style(styles.adiz_style());

            for(const auto& a : adizs)
            {
                for(const auto& part : a.parts)
                {
                    append_polygon_ring(poly[layer_adiz], part, ls);
                }
            }
        }

        void build_artcc_polylines(double lon_min, double lat_min,
                                    double lon_max, double lat_max, double z)
        {
            const auto& artccs = db.query_artcc(lon_min, lat_min, lon_max, lat_max);

            for(const auto& a : artccs)
            {
                if(!styles.artcc_visible(a.altitude, z)) continue;
                auto ls = to_line_style(styles.artcc_style(a.altitude));

                append_polygon_ring(poly[layer_artcc], a.points, ls);
            }
        }

        void build_obstacle_vertices(double lon_min, double lat_min,
                                      double lon_max, double lat_max, double z)
        {
            const auto& obstacles = db.query_obstacles(lon_min, lat_min, lon_max, lat_max);
            float radius = static_cast<float>(half_extent_y * SYMBOL_RADIUS_OBSTACLE);

            for(const auto& obs : obstacles)
            {
                if(!styles.obstacle_visible(obs.agl_ht, z))
                {
                    continue;
                }

                const auto& cs = styles.obstacle_style(obs.agl_ht);
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
                if(!styles.airspace_visible(arsp.airspace_class, arsp.local_type, z)) continue;
                auto ls = to_line_style(styles.airspace_style(arsp.airspace_class, arsp.local_type));

                for(const auto& ring : arsp.parts)
                {
                    append_polygon_ring(poly[layer_airspace], ring.points, ls);
                }
            }
        }

        void rebuild_sdf_lines()
        {
            std::vector<std::vector<glm::vec2>> all_polylines;
            std::vector<line_style> all_styles;

            for(int i = 0; i < layer_sdf_count; i++)
            {
                if(!vis[i]) continue;
                all_polylines.insert(all_polylines.end(),
                    poly[i].polylines.begin(), poly[i].polylines.end());
                all_styles.insert(all_styles.end(),
                    poly[i].styles.begin(), poly[i].styles.end());
            }

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
                                   double half_extent_y, int viewport_height,
                                   double aspect_ratio)
    {
        pimpl->center_x = (vx_min + vx_max) * 0.5;
        pimpl->center_y = (vy_min + vy_max) * 0.5;
        pimpl->half_extent_y = half_extent_y;
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

            if(pimpl->vis[layer_obstacles]) draw(pimpl->obstacle_buffer);
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
