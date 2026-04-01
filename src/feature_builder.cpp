#include "feature_builder.hpp"
#include "chart_style.hpp"
#include "map_view.hpp"
#include "nasr_database.hpp"
#include <cmath>
#include <cstring>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_set>
#include <glm/glm.hpp>
#include <vector>

namespace nasrbrowse
{
    // Convert feature_style to line_style for SDF line features
    static line_style to_line_style(const feature_style& fs)
    {
        return {fs.line_width, fs.border_width, fs.dash_length, fs.gap_length,
                fs.r, fs.g, fs.b, fs.a, 0};
    }

    // Symbol base radii as fraction of view half-extent
    constexpr double SYMBOL_RADIUS_AIRPORT  = 0.012;
    constexpr double SYMBOL_RADIUS_FIX      = 0.012;
    constexpr double SYMBOL_RADIUS_OBSTACLE = 0.012;
    constexpr double SYMBOL_RADIUS_COMM     = 0.012;

    // Vector letter sizing (shared by airport and PJA icons)
    constexpr float LETTER_HEIGHT    = 0.385F;
    constexpr float LETTER_ASPECT    = 0.7F;
    constexpr float LETTER_WIDTH_PX  = 2.0F;

    // Interior fill for outlined+filled symbols (airports, PJA diamonds)
    constexpr float SYMBOL_FILL_PX   = 50.0F;

    // Query bbox padding (fraction of view extent)
    constexpr double QUERY_BBOX_PADDING = 0.5;

    struct feature_builder::impl
    {
        nasr_database db;
        chart_style styles;

        mutable std::mutex mutex;
        std::condition_variable cv;
        std::optional<feature_build_request> pending_request;
        std::optional<feature_build_result> completed_result;
        std::thread worker;
        bool shutdown = false;

        // Intermediate build state (worker-only, no lock needed)
        std::vector<glm::vec2> navaid_positions;
        float navaid_clearance = 0;
        std::unordered_set<std::string> airway_waypoints;

        // Temporary poly array built by worker (moved to result when done)
        std::array<polyline_data, layer_sdf_count> poly;

        impl(const char* db_path, const chart_style& cs)
            : db(db_path)
            , styles(cs)
        {
        }

        void worker_loop()
        {
            while(true)
            {
                feature_build_request req;
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    cv.wait(lock, [this] { return shutdown || pending_request.has_value(); });
                    if(shutdown)
                    {
                        return;
                    }
                    req = *pending_request;
                    pending_request.reset();
                }

                build_vertices(req);

                feature_build_result result;
                result.poly = std::move(poly);

                {
                    std::lock_guard<std::mutex> lock(mutex);
                    completed_result = std::move(result);
                }
            }
        }

        void build_vertices(const feature_build_request& req)
        {
            double lon_pad = (req.lon_max - req.lon_min) * QUERY_BBOX_PADDING;
            double lat_pad = (req.lat_max - req.lat_min) * QUERY_BBOX_PADDING;
            double qlon_min = req.lon_min - lon_pad;
            double qlat_min = req.lat_min - lat_pad;
            double qlon_max = req.lon_max + lon_pad;
            double qlat_max = req.lat_max + lat_pad;

            for(auto& p : poly) p.clear();

            build_airport_polylines(qlon_min, qlat_min, qlon_max, qlat_max, req);
            build_navaid_polylines(qlon_min, qlat_min, qlon_max, qlat_max, req);
            build_airway_polylines(qlon_min, qlat_min, qlon_max, qlat_max, req);
            build_mtr_polylines(qlon_min, qlat_min, qlon_max, qlat_max, req);
            build_sua_polylines(qlon_min, qlat_min, qlon_max, qlat_max, req);
            build_pja_polylines(qlon_min, qlat_min, qlon_max, qlat_max, req);
            build_maa_polylines(qlon_min, qlat_min, qlon_max, qlat_max, req);
            build_adiz_polylines(qlon_min, qlat_min, qlon_max, qlat_max, req);
            build_artcc_polylines(qlon_min, qlat_min, qlon_max, qlat_max, req);
            build_obstacle_polylines(qlon_min, qlat_min, qlon_max, qlat_max, req);
            build_rco_polylines(qlon_min, qlat_min, qlon_max, qlat_max, req);
            build_awos_polylines(qlon_min, qlat_min, qlon_max, qlat_max, req);
            build_fix_polylines(qlon_min, qlat_min, qlon_max, qlat_max, req);
            build_runway_polylines(qlon_min, qlat_min, qlon_max, qlat_max, req);
            build_airspace_polylines(qlon_min, qlat_min, qlon_max, qlat_max, req);
        }

        // --- Geometry helpers ---

        void add_obstacle_polylines(polyline_data& pd, float cx, float cy,
                                     float radius, bool tall, bool lighted,
                                     const line_style& ls)
        {
            float dot_r = radius * 0.2F;
            float half_w = radius * 0.7F;
            float leg_y = cy - dot_r;
            float apex_y = leg_y + radius * 1.6F;
            float mast_top = tall ? apex_y + radius * 0.8F : apex_y;

            if(tall)
            {
                pd.polylines.push_back({
                    glm::vec2(cx - half_w, leg_y),
                    glm::vec2(cx, apex_y),
                    glm::vec2(cx, mast_top),
                    glm::vec2(cx, apex_y),
                    glm::vec2(cx + half_w, leg_y),
                });
            }
            else
            {
                pd.polylines.push_back({
                    glm::vec2(cx - half_w, leg_y),
                    glm::vec2(cx, apex_y),
                    glm::vec2(cx + half_w, leg_y),
                });
            }
            pd.styles.push_back(ls);

            add_circle_to(pd, cx, cy, dot_r, ls, 8);

            if(lighted)
            {
                float gap = radius * 0.4F;
                float ray_len = radius * 0.15F;
                constexpr float DEG_TO_RAD = static_cast<float>(M_PI) / 180.0F;
                float angles[] = {-120, -60, 0, 60, 120};
                for(float deg : angles)
                {
                    float rad = (90.0F - deg) * DEG_TO_RAD;
                    float dx = std::cos(rad);
                    float dy = std::sin(rad);
                    float x0 = cx + dx * gap;
                    float y0 = mast_top + dy * gap;
                    float x1 = cx + dx * (gap + ray_len);
                    float y1 = mast_top + dy * (gap + ray_len);
                    add_seg_to(pd, x0, y0, x1, y1, ls);
                }
            }
        }

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

        void add_seg_to(polyline_data& pd, float x0, float y0,
                        float x1, float y1, const line_style& ls)
        {
            pd.polylines.push_back({glm::vec2(x0, y0), glm::vec2(x1, y1)});
            pd.styles.push_back(ls);
        }

        // --- Letter data ---

        struct letter_def
        {
            const float* segs;
            int count;
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

        // --- Classification helpers ---

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

        bool fix_on_airway(const std::string& fix_id) const
        {
            return airway_waypoints.find(fix_id) != airway_waypoints.end();
        }

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

        static const letter_def* maa_letter(const std::string& type)
        {
            if(type == "AEROBATIC PRACTICE") return &letter_A;
            if(type == "GLIDER")             return &letter_G;
            if(type == "HANG GLIDER")        return &letter_H;
            if(type == "ULTRALIGHT")         return &letter_U;
            if(type == "SPACE LAUNCH")       return &letter_S;
            return nullptr;
        }

        // --- Navaid symbol geometry builders ---

        void add_hexagon(polyline_data& pd, float cx, float cy, float r, const line_style& ls)
        {
            std::vector<glm::vec2> pts;
            for(int i = 0; i < 6; i++)
            {
                float angle = glm::radians(60.0F * i);
                pts.emplace_back(cx + r * std::cos(angle), cy + r * std::sin(angle));
            }
            pts.push_back(pts.front());
            pd.polylines.push_back(std::move(pts));
            pd.styles.push_back(ls);
        }

        void add_center_dot(polyline_data& pd, float cx, float cy, float r, const line_style& ls)
        {
            float d = r * 0.15F;
            pd.polylines.push_back({
                {cx - d, cy}, {cx, cy + d}, {cx + d, cy}, {cx, cy - d}, {cx - d, cy}
            });
            pd.styles.push_back(ls);
        }

        void add_rect(polyline_data& pd, float cx, float cy, float hw, float hh, const line_style& ls)
        {
            pd.polylines.push_back({
                {cx - hw, cy - hh}, {cx + hw, cy - hh},
                {cx + hw, cy + hh}, {cx - hw, cy + hh}, {cx - hw, cy - hh}
            });
            pd.styles.push_back(ls);
        }

        void add_caltrop(polyline_data& pd, float cx, float cy, float hex_r, const line_style& ls)
        {
            float vx[6], vy[6];
            for(int i = 0; i < 6; i++)
            {
                float angle = glm::radians(60.0F * i);
                vx[i] = cx + hex_r * std::cos(angle);
                vy[i] = cy + hex_r * std::sin(angle);
            }

            int edges[][2] = {{0, 1}, {2, 3}, {4, 5}};
            float normal_angles[] = {30.0F, 150.0F, 270.0F};
            float h = hex_r * 0.5F;

            for(int e = 0; e < 3; e++)
            {
                int a = edges[e][0], b = edges[e][1];
                float na = glm::radians(normal_angles[e]);
                float nx = std::cos(na) * h;
                float ny = std::sin(na) * h;

                pd.polylines.push_back({
                    {vx[a], vy[a]},
                    {vx[b], vy[b]},
                    {vx[b] + nx, vy[b] + ny},
                    {vx[a] + nx, vy[a] + ny},
                    {vx[a], vy[a]},
                });
                pd.styles.push_back(ls);
            }
        }

        void add_circle(polyline_data& pd, float cx, float cy, float r, const line_style& ls)
        {
            const int n = 16;
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

        void add_waypoint_star_polyline(polyline_data& pd,
                                         float cx, float cy, float r, const line_style& ls)
        {
            constexpr int ARC_SEGS = 3;
            constexpr float PI = 3.14159265F;
            constexpr float SQRT2_M1 = 0.41421356F;

            struct arc_def { float acx, acy; float start_angle; };
            const arc_def arcs[4] = {
                { r,  r, -PI / 2},
                {-r,  r,  0},
                {-r, -r,  PI / 2},
                { r, -r,  PI},
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

        void add_comm_symbol(polyline_data& pd, float cx, float cy,
                              float radius, const line_style& ls)
        {
            add_circle_to(pd, cx, cy, radius, ls);
            float dot_r = radius * 0.2F;
            add_circle_to(pd, cx, cy, dot_r, ls, 8);
        }

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

        // --- Feature build methods ---

        void build_airport_polylines(double lon_min, double lat_min,
                                      double lon_max, double lat_max,
                                      const feature_build_request& req)
        {
            const auto& airports = db.query_airports(lon_min, lat_min, lon_max, lat_max);
            constexpr float APT_OUTER_SCALE = 1.2F;
            constexpr float APT_RING_WIDTH_PX = 1.0F;
            constexpr float APT_FILL_RADIUS = 0.5F;

            float r = static_cast<float>(req.half_extent_y * SYMBOL_RADIUS_AIRPORT);
            float pixels_per_world = static_cast<float>(req.viewport_height / (2.0 * req.half_extent_y));

            for(const auto& apt : airports)
            {
                if(!styles.airport_visible(apt, req.zoom))
                {
                    continue;
                }

                auto& pd = poly[layer_airports];

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
                    add_circle_to(pd, cx, cy, ring_geom_r, ring_ls);
                }
            }
        }

        void build_navaid_polylines(double lon_min, double lat_min,
                                     double lon_max, double lat_max,
                                     const feature_build_request& req)
        {
            constexpr float NAV_NDB_CIRCLE = 0.4F;
            constexpr float NAV_DME_RECT = 0.85F;
            constexpr float NAV_VORDME_WIDTH = 1.1F;
            constexpr float NAV_CLEARANCE = 2.0F;

            const auto& navaids = db.query_navaids(lon_min, lat_min, lon_max, lat_max);
            float r = static_cast<float>(req.half_extent_y * SYMBOL_RADIUS_AIRPORT);

            navaid_positions.clear();
            navaid_clearance = r * NAV_CLEARANCE;

            for(const auto& nav : navaids)
            {
                if(nav.nav_type == "VOT" || nav.nav_type == "FAN MARKER" ||
                   nav.nav_type == "MARINE NDB")
                {
                    continue;
                }

                if(!styles.navaid_visible(nav.nav_type, req.zoom))
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

                auto& pd = poly[layer_navaids];

                if(nav.nav_type == "NDB")
                {
                    add_circle(pd, cx, cy, r * NAV_NDB_CIRCLE, filled_ls);
                }
                else if(nav.nav_type == "NDB/DME")
                {
                    add_circle(pd, cx, cy, r * NAV_NDB_CIRCLE, filled_ls);
                    add_rect(pd, cx, cy, r * NAV_DME_RECT, r * NAV_DME_RECT, ls);
                }
                else if(nav.nav_type == "DME")
                {
                    add_rect(pd, cx, cy, r * NAV_DME_RECT, r * NAV_DME_RECT, filled_ls);
                }
                else if(std::strcmp(nav.nav_type.c_str(), "VOR/DME") == 0)
                {
                    add_hexagon(pd, cx, cy, r, filled_ls);
                    add_center_dot(pd, cx, cy, r, ls);
                    add_rect(pd, cx, cy, r * NAV_VORDME_WIDTH, r * NAV_DME_RECT, ls);
                }
                else if(nav.nav_type == "VORTAC" || nav.nav_type == "TACAN")
                {
                    add_hexagon(pd, cx, cy, r, filled_ls);
                    add_center_dot(pd, cx, cy, r, ls);
                    add_caltrop(pd, cx, cy, r, ls);
                }
                else
                {
                    add_hexagon(pd, cx, cy, r, filled_ls);
                    add_center_dot(pd, cx, cy, r, ls);
                }
            }
        }

        void build_airway_polylines(double lon_min, double lat_min,
                                     double lon_max, double lat_max,
                                     const feature_build_request& req)
        {
            airway_waypoints.clear();
            const auto& airways = db.query_airways(lon_min, lat_min, lon_max, lat_max);

            for(const auto& seg : airways)
            {
                if(!styles.airway_visible(seg.awy_id, req.zoom))
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

                if((x1 - x0) * dx + (y1 - y0) * dy <= 0.0F)
                {
                    continue;
                }

                poly[layer_airways].polylines.push_back({glm::vec2(x0, y0), glm::vec2(x1, y1)});
                poly[layer_airways].styles.push_back(to_line_style(fs));
            }
        }

        void build_fix_polylines(double lon_min, double lat_min,
                                  double lon_max, double lat_max,
                                  const feature_build_request& req)
        {
            const auto& fixes = db.query_fixes(lon_min, lat_min, lon_max, lat_max);
            float radius = static_cast<float>(req.half_extent_y * SYMBOL_RADIUS_FIX);

            for(const auto& fix : fixes)
            {
                if(!styles.fix_visible(fix_on_airway(fix.fix_id), req.zoom))
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

        void build_mtr_polylines(double lon_min, double lat_min,
                                  double lon_max, double lat_max,
                                  const feature_build_request& req)
        {
            if(!styles.mtr_visible(req.zoom))
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
                                     double lon_max, double lat_max,
                                     const feature_build_request& req)
        {
            if(!styles.runway_visible(req.zoom))
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

        void build_sua_polylines(double lon_min, double lat_min,
                                  double lon_max, double lat_max,
                                  const feature_build_request& req)
        {
            const auto& suas = db.query_sua(lon_min, lat_min, lon_max, lat_max);

            for(const auto& s : suas)
            {
                if(!styles.sua_visible(s.sua_type, req.zoom)) continue;
                auto ls = to_line_style(styles.sua_style(s.sua_type));

                for(const auto& ring : s.parts)
                {
                    append_polygon_ring(poly[layer_sua], ring.points, ls);
                }
            }
        }

        void build_pja_polylines(double lon_min, double lat_min,
                                  double lon_max, double lat_max,
                                  const feature_build_request& req)
        {
            bool area_vis = styles.pja_area_visible(req.zoom);
            bool point_vis = styles.pja_point_visible(req.zoom);
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
                    float r = static_cast<float>(req.half_extent_y * SYMBOL_RADIUS_PJA);

                    auto ls = to_line_style(styles.pja_point_style());
                    ls.fill_width = SYMBOL_FILL_PX;
                    poly[layer_pja].polylines.push_back({
                        {cx + r, cy}, {cx, cy + r}, {cx - r, cy},
                        {cx, cy - r}, {cx + r, cy},
                    });
                    poly[layer_pja].styles.push_back(ls);

                    float lh = r * LETTER_HEIGHT;
                    float lw = lh * LETTER_ASPECT;
                    line_style white_ls = {LETTER_WIDTH_PX, 0, 0, 0, 1, 1, 1, 1, 0};
                    draw_letter(poly[layer_pja], letter_P, cx, cy, lw, lh, white_ls);
                }
            }
        }

        void build_maa_polylines(double lon_min, double lat_min,
                                  double lon_max, double lat_max,
                                  const feature_build_request& req)
        {
            bool area_vis = styles.maa_area_visible(req.zoom);
            bool point_vis = styles.maa_point_visible(req.zoom);
            if(!area_vis && !point_vis) return;

            constexpr int CIRCLE_SEGS = 24;
            constexpr double PI = 3.14159265358979;
            constexpr double NM_TO_DEG_LAT = 1.0 / 60.0;

            const auto& maas = db.query_maas(lon_min, lat_min, lon_max, lat_max);

            for(const auto& m : maas)
            {
                if(!m.shape.empty() && area_vis)
                {
                    auto ls = to_line_style(styles.maa_area_style());
                    append_polygon_ring(poly[layer_maa], m.shape, ls);
                }
                else if(m.radius_nm > 0.0 && area_vis)
                {
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
                    float cx = static_cast<float>(lon_to_mx(m.lon));
                    float cy = static_cast<float>(lat_to_my(m.lat));
                    float r = static_cast<float>(req.half_extent_y * SYMBOL_RADIUS_AIRPORT);

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
                                   double lon_max, double lat_max,
                                   const feature_build_request& req)
        {
            if(!styles.adiz_visible(req.zoom)) return;

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
                                    double lon_max, double lat_max,
                                    const feature_build_request& req)
        {
            const auto& artccs = db.query_artcc(lon_min, lat_min, lon_max, lat_max);

            for(const auto& a : artccs)
            {
                if(!styles.artcc_visible(a.altitude, req.zoom)) continue;
                auto ls = to_line_style(styles.artcc_style(a.altitude));

                append_polygon_ring(poly[layer_artcc], a.points, ls);
            }
        }

        void build_obstacle_polylines(double lon_min, double lat_min,
                                       double lon_max, double lat_max,
                                       const feature_build_request& req)
        {
            const auto& obstacles = db.query_obstacles(lon_min, lat_min, lon_max, lat_max);
            float radius = static_cast<float>(req.half_extent_y * SYMBOL_RADIUS_OBSTACLE);

            for(const auto& obs : obstacles)
            {
                if(!styles.obstacle_visible(obs.agl_ht, req.zoom))
                {
                    continue;
                }

                auto ls = to_line_style(styles.obstacle_style(obs.agl_ht));
                float cx = static_cast<float>(lon_to_mx(obs.lon));
                float cy = static_cast<float>(lat_to_my(obs.lat));
                bool lighted = obs.lighting != "N";
                add_obstacle_polylines(poly[layer_obstacles], cx, cy, radius,
                                       obs.agl_ht >= 1000, lighted, ls);
            }
        }

        void build_airspace_polylines(double lon_min, double lat_min,
                                       double lon_max, double lat_max,
                                       const feature_build_request& req)
        {
            const auto& airspaces = db.query_class_airspace(lon_min, lat_min, lon_max, lat_max);

            for(const auto& arsp : airspaces)
            {
                if(!styles.airspace_visible(arsp.airspace_class, arsp.local_type, req.zoom)) continue;
                auto ls = to_line_style(styles.airspace_style(arsp.airspace_class, arsp.local_type));

                for(const auto& ring : arsp.parts)
                {
                    append_polygon_ring(poly[layer_airspace], ring.points, ls);
                }
            }
        }

        template<typename T>
        void build_comm_polylines(const std::vector<T>& features,
                                   int layer_id, const line_style& ls,
                                   const feature_build_request& req)
        {
            float radius = static_cast<float>(req.half_extent_y * SYMBOL_RADIUS_COMM);
            for(const auto& f : features)
            {
                float cx = static_cast<float>(lon_to_mx(f.lon));
                float cy = static_cast<float>(lat_to_my(f.lat));
                add_comm_symbol(poly[layer_id], cx, cy, radius*0.75F, ls);
            }
        }

        void build_rco_polylines(double lon_min, double lat_min,
                                  double lon_max, double lat_max,
                                  const feature_build_request& req)
        {
            if(!styles.rco_visible(req.zoom)) return;
            build_comm_polylines(db.query_comm_outlets(lon_min, lat_min, lon_max, lat_max),
                                 layer_rco, to_line_style(styles.rco_style()), req);
        }

        void build_awos_polylines(double lon_min, double lat_min,
                                   double lon_max, double lat_max,
                                   const feature_build_request& req)
        {
            if(!styles.awos_visible(req.zoom)) return;
            build_comm_polylines(db.query_awos(lon_min, lat_min, lon_max, lat_max),
                                 layer_awos, to_line_style(styles.awos_style()), req);
        }
    };

    feature_builder::feature_builder(const char* db_path, const chart_style& cs)
        : pimpl(new impl(db_path, cs))
    {
        pimpl->worker = std::thread(&impl::worker_loop, pimpl.get());
    }

    feature_builder::~feature_builder()
    {
        {
            std::lock_guard<std::mutex> lock(pimpl->mutex);
            pimpl->shutdown = true;
        }
        pimpl->cv.notify_one();
        pimpl->worker.join();
    }

    void feature_builder::request(const feature_build_request& req)
    {
        std::lock_guard<std::mutex> lock(pimpl->mutex);
        pimpl->pending_request = req;
        pimpl->cv.notify_one();
    }

    std::optional<feature_build_result> feature_builder::drain_result()
    {
        std::lock_guard<std::mutex> lock(pimpl->mutex);
        std::optional<feature_build_result> result;
        result.swap(pimpl->completed_result);
        return result;
    }

} // namespace nasrbrowse
