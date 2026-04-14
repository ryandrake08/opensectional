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
#include <mapbox/earcut.hpp>
#include <vector>

// Tell earcut how to read glm::vec2
namespace mapbox {
namespace util {
    template <> struct nth<0, glm::vec2> { static float get(const glm::vec2& p) { return p.x; } };
    template <> struct nth<1, glm::vec2> { static float get(const glm::vec2& p) { return p.y; } };
}}

namespace nasrbrowse
{
    // Triangulate a polygon (outer ring + optional hole rings) into a flat
    // triangle list appended to `out`. All vertices share `color`.
    static void triangulate_polygon(
        const std::vector<glm::vec2>& outer,
        const std::vector<std::vector<glm::vec2>>& holes,
        const glm::vec4& color,
        polygon_fill_data& out)
    {
        if(outer.size() < 3) return;

        std::vector<std::vector<glm::vec2>> rings;
        rings.reserve(1 + holes.size());
        rings.push_back(outer);
        for(const auto& h : holes) rings.push_back(h);

        // earcut returns flat indices into the concatenated rings
        std::vector<uint32_t> indices = mapbox::earcut<uint32_t>(rings);

        // Build flat vertex list for lookup
        std::vector<glm::vec2> flat;
        flat.reserve(outer.size() + [&]{ size_t s = 0; for(auto& h : holes) s += h.size(); return s; }());
        flat.insert(flat.end(), outer.begin(), outer.end());
        for(const auto& h : holes) flat.insert(flat.end(), h.begin(), h.end());

        out.triangles.reserve(out.triangles.size() + indices.size());
        for(uint32_t idx : indices)
        {
            out.triangles.push_back({flat[idx], color});
        }
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
    constexpr double SYMBOL_RADIUS_OBSTACLE = 0.012;
    constexpr double SYMBOL_RADIUS_COMM     = 0.012;

    // Label placement priorities (higher = placed first in overlap elimination)
    constexpr int LABEL_PRIORITY_AIRPORT_TOWERED   = 100;
    constexpr int LABEL_PRIORITY_AIRPORT_UNTOWERED = 80;
    constexpr int LABEL_PRIORITY_NAVAID_VOR        = 60;
    constexpr int LABEL_PRIORITY_NAVAID_NDB        = 40;
    constexpr int LABEL_PRIORITY_FIX_AIRWAY        = 30;
    constexpr int LABEL_PRIORITY_FIX_OTHER         = 20;

    // Vector letter sizing (shared by airport and PJA icons)
    constexpr float LETTER_HEIGHT    = 0.385F;
    constexpr float LETTER_ASPECT    = 0.7F;
    constexpr float LETTER_WIDTH_PX  = 2.0F;

    // Interior fill for outlined+filled symbols (airports, PJA diamonds)
    constexpr float SYMBOL_FILL_PX   = 50.0F;

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

        // Label candidates collected during build (world-space Mercator coords)
        struct world_label
        {
            std::string text;
            double mx, my;      // Mercator meters
            int priority;
            int layer;
        };
        std::vector<world_label> world_labels;

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
                if(req.selection)
                    build_selection_overlay(req, *req.selection,
                                            result.selection_overlay,
                                            result.selection_fill);

                // Move raw world labels; overlap elimination happens on main thread
                result.labels.reserve(world_labels.size());
                for(auto& wl : world_labels)
                {
                    result.labels.push_back(
                        {std::move(wl.text), wl.mx, wl.my, wl.priority, wl.layer});
                }

                {
                    std::lock_guard<std::mutex> lock(mutex);
                    completed_result = std::move(result);
                }
            }
        }

        void build_all_features(const geo_bbox& bbox,
                                 const feature_build_request& req,
                                 double mx_offset)
        {
            build_airport_polylines(bbox, req, mx_offset);
            build_navaid_polylines(bbox, req, mx_offset);
            build_airway_polylines(bbox, req, mx_offset);
            build_mtr_polylines(bbox, req, mx_offset);
            build_sua_polylines(bbox, req, mx_offset);
            build_pja_polylines(bbox, req, mx_offset);
            build_maa_polylines(bbox, req, mx_offset);
            build_adiz_polylines(bbox, req, mx_offset);
            build_artcc_polylines(bbox, req, mx_offset);
            build_obstacle_polylines(bbox, req, mx_offset);
            build_rco_polylines(bbox, req, mx_offset);
            build_awos_polylines(bbox, req, mx_offset);
            build_fix_polylines(bbox, req, mx_offset);
            build_runway_polylines(bbox, req, mx_offset);
            build_airspace_polylines(bbox, req, mx_offset);
        }

        // Draw order: halo first, icon body second. Labels come from the
        // normal label pipeline and already render on top.
        void build_selection_overlay(const feature_build_request& req,
                                      const pick_feature& sel,
                                      polyline_data& out,
                                      polygon_fill_data& fill_out)
        {
            // Halo radius = largest point-symbol radius × 1.5, same for all
            // point features so the halo is uniform regardless of feature type.
            constexpr float HALO_SCALE = 1.5F * 1.2F; // airport APT_OUTER_SCALE = 1.2

            float r_base = static_cast<float>(req.half_extent_y * SYMBOL_RADIUS_AIRPORT);
            float pixels_per_world = static_cast<float>(req.viewport_height / (2.0 * req.half_extent_y));
            float halo_r = r_base * HALO_SCALE;

            // Convert an airspace_point ring to Mercator glm::vec2. Skips
            // empty input and ensures the ring is closed (last == first).
            auto ring_to_mercator = [](const std::vector<airspace_point>& pts,
                                        std::vector<glm::vec2>& out_ring)
            {
                out_ring.clear();
                out_ring.reserve(pts.size());
                for(const auto& p : pts)
                {
                    out_ring.emplace_back(
                        static_cast<float>(lon_to_mx(p.lon)),
                        static_cast<float>(lat_to_my(p.lat)));
                }
            };

            // Sample a circle into a polygon ring (Mercator vec2, 48 points).
            auto circle_to_ring = [](double center_lon, double center_lat,
                                      double radius_nm,
                                      std::vector<glm::vec2>& out_ring)
            {
                constexpr int N = 48;
                constexpr double NM_TO_M = 1852.0;
                double lat_rad = center_lat * M_PI / 180.0;
                double r = radius_nm * NM_TO_M / std::cos(lat_rad);
                double cx = lon_to_mx(center_lon);
                double cy = lat_to_my(center_lat);
                out_ring.clear();
                out_ring.reserve(N + 1);
                for(int i = 0; i <= N; i++)
                {
                    double a = 2.0 * M_PI * i / N;
                    out_ring.emplace_back(
                        static_cast<float>(cx + r * std::cos(a)),
                        static_cast<float>(cy + r * std::sin(a)));
                }
            };

            // Emit a closed boundary polyline (white, no border, widened to
            // cover the feature's normal outline like the airway stage).
            // Polygon rings in the DB are stored closed (first == last).
            auto emit_boundary = [&](const std::vector<glm::vec2>& ring,
                                      const line_style& base)
            {
                if(ring.size() < 2) return;
                line_style ls = base;
                ls.r = ls.g = ls.b = 1.0F; ls.a = 1.0F;
                ls.line_width = ls.line_width + 2.0F * ls.border_width + 2.0F;
                ls.border_width = 0; ls.dash_length = 0; ls.gap_length = 0;
                out.polylines.push_back(ring);
                out.styles.push_back(ls);
            };

            auto emit_halo = [&](float cx, float cy)
            {
                // Filled white disc: line at radius halo_r/2 with line_width =
                // halo_r * pixels_per_world covers the full interior.
                float fill_px = halo_r * pixels_per_world;
                line_style halo_ls = {fill_px, 0, 0, 0, 1, 1, 1, 1, 0};
                add_circle_to(out, cx, cy, halo_r * 0.5F, halo_ls);
            };

            std::visit([&](const auto& f)
            {
                using T = std::decay_t<decltype(f)>;
                if constexpr(std::is_same_v<T, airport>)
                {
                    float cx = static_cast<float>(lon_to_mx(f.lon));
                    float cy = static_cast<float>(lat_to_my(f.lat));
                    emit_halo(cx, cy);
                    emit_airport_icon(out, cx, cy, r_base, pixels_per_world,
                                      f, styles.airport_style(f));
                }
                else if constexpr(std::is_same_v<T, navaid>)
                {
                    float cx = static_cast<float>(lon_to_mx(f.lon));
                    float cy = static_cast<float>(lat_to_my(f.lat));
                    emit_halo(cx, cy);
                    emit_navaid_icon(out, cx, cy, r_base, f, styles.navaid_style(f.nav_type));
                }
                else if constexpr(std::is_same_v<T, fix>)
                {
                    float cx = static_cast<float>(lon_to_mx(f.lon));
                    float cy = static_cast<float>(lat_to_my(f.lat));
                    emit_halo(cx, cy);
                    emit_fix_icon(out, cx, cy, r_base, f, styles.fix_style(f.use_code));
                }
                else if constexpr(std::is_same_v<T, obstacle>)
                {
                    float cx = static_cast<float>(lon_to_mx(f.lon));
                    float cy = static_cast<float>(lat_to_my(f.lat));
                    emit_halo(cx, cy);
                    emit_obstacle_icon(out, cx, cy, r_base, f, styles.obstacle_style(f.agl_ht));
                }
                else if constexpr(std::is_same_v<T, pja>)
                {
                    if(f.radius_nm > 0.0)
                    {
                        auto fs = styles.pja_area_style();
                        line_style base = to_line_style(fs);
                        glm::vec4 fill_color{fs.r, fs.g, fs.b, 0.25F};
                        std::vector<glm::vec2> ring;
                        circle_to_ring(f.lon, f.lat, f.radius_nm, ring);
                        emit_boundary(ring, base);
                        triangulate_polygon(ring, {}, fill_color, fill_out);
                    }
                    else
                    {
                        float cx = static_cast<float>(lon_to_mx(f.lon));
                        float cy = static_cast<float>(lat_to_my(f.lat));
                        emit_halo(cx, cy);
                        emit_pja_point_icon(out, cx, cy, r_base, styles.pja_point_style());
                    }
                }
                else if constexpr(std::is_same_v<T, maa>)
                {
                    if(!f.shape.empty() || f.radius_nm > 0.0)
                    {
                        auto fs = styles.maa_area_style();
                        line_style base = to_line_style(fs);
                        glm::vec4 fill_color{fs.r, fs.g, fs.b, 0.25F};
                        std::vector<glm::vec2> ring;
                        if(!f.shape.empty())
                            ring_to_mercator(f.shape, ring);
                        else
                            circle_to_ring(f.lon, f.lat, f.radius_nm, ring);
                        emit_boundary(ring, base);
                        triangulate_polygon(ring, {}, fill_color, fill_out);
                    }
                    else
                    {
                        float cx = static_cast<float>(lon_to_mx(f.lon));
                        float cy = static_cast<float>(lat_to_my(f.lat));
                        emit_halo(cx, cy);
                        emit_maa_point_icon(out, cx, cy, r_base, f, styles.maa_point_style());
                    }
                }
                else if constexpr(std::is_same_v<T, awos>)
                {
                    float cx = static_cast<float>(lon_to_mx(f.lon));
                    float cy = static_cast<float>(lat_to_my(f.lat));
                    emit_halo(cx, cy);
                    emit_comm_icon(out, cx, cy, r_base, to_line_style(styles.awos_style()));
                }
                else if constexpr(std::is_same_v<T, comm_outlet>)
                {
                    float cx = static_cast<float>(lon_to_mx(f.lon));
                    float cy = static_cast<float>(lat_to_my(f.lat));
                    emit_halo(cx, cy);
                    emit_comm_icon(out, cx, cy, r_base, to_line_style(styles.rco_style()));
                }
                else if constexpr(std::is_same_v<T, airway_segment>)
                {
                    auto fs = styles.airway_style(f.awy_id);
                    line_style ls = to_line_style(fs);
                    ls.r = ls.g = ls.b = 1.0F; ls.a = 1.0F;
                    // Widen to cover the original line's border on both sides.
                    ls.line_width = ls.line_width + 2.0F * ls.border_width + 2.0F;
                    ls.border_width = 0; ls.dash_length = 0; ls.gap_length = 0;
                    for(const auto& seg : db.query_airway_by_id(f.awy_id))
                    {
                        float x0 = static_cast<float>(lon_to_mx(seg.from_lon));
                        float y0 = static_cast<float>(lat_to_my(seg.from_lat));
                        float x1 = static_cast<float>(lon_to_mx(seg.to_lon));
                        float y1 = static_cast<float>(lat_to_my(seg.to_lat));
                        out.polylines.push_back({{x0, y0}, {x1, y1}});
                        out.styles.push_back(ls);
                    }
                }
                else if constexpr(std::is_same_v<T, mtr_segment>)
                {
                    line_style ls = to_line_style(styles.mtr_style());
                    ls.r = ls.g = ls.b = 1.0F; ls.a = 1.0F;
                    ls.line_width = ls.line_width + 2.0F * ls.border_width + 2.0F;
                    ls.border_width = 0; ls.dash_length = 0; ls.gap_length = 0;
                    for(const auto& seg : db.query_mtr_by_id(f.mtr_id))
                    {
                        float x0 = static_cast<float>(lon_to_mx(seg.from_lon));
                        float y0 = static_cast<float>(lat_to_my(seg.from_lat));
                        float x1 = static_cast<float>(lon_to_mx(seg.to_lon));
                        float y1 = static_cast<float>(lat_to_my(seg.to_lat));
                        out.polylines.push_back({{x0, y0}, {x1, y1}});
                        out.styles.push_back(ls);
                    }
                }
                else if constexpr(std::is_same_v<T, class_airspace>)
                {
                    auto fs = styles.airspace_style(f.airspace_class, f.local_type);
                    line_style base = to_line_style(fs);
                    glm::vec4 fill_color{fs.r, fs.g, fs.b, 0.25F};

                    // Group into outer + its holes, triangulate per outer.
                    std::vector<glm::vec2> outer;
                    std::vector<std::vector<glm::vec2>> holes;
                    auto flush = [&]()
                    {
                        if(!outer.empty())
                            triangulate_polygon(outer, holes, fill_color, fill_out);
                        outer.clear(); holes.clear();
                    };
                    std::vector<glm::vec2> ring;
                    for(const auto& part : f.parts)
                    {
                        ring_to_mercator(part.points, ring);
                        emit_boundary(ring, base);
                        if(part.is_hole) holes.push_back(ring);
                        else { flush(); outer = ring; }
                    }
                    flush();
                }
                else if constexpr(std::is_same_v<T, sua>)
                {
                    auto fs = styles.sua_style(f.sua_type);
                    line_style base = to_line_style(fs);
                    glm::vec4 fill_color{fs.r, fs.g, fs.b, 0.25F};

                    std::vector<glm::vec2> outer;
                    std::vector<std::vector<glm::vec2>> holes;
                    auto flush = [&]()
                    {
                        if(!outer.empty())
                            triangulate_polygon(outer, holes, fill_color, fill_out);
                        outer.clear(); holes.clear();
                    };
                    std::vector<glm::vec2> ring;
                    for(const auto& part : f.parts)
                    {
                        if(part.is_circle)
                            circle_to_ring(part.circle_lon, part.circle_lat,
                                            part.circle_radius_nm, ring);
                        else
                            ring_to_mercator(part.points, ring);
                        emit_boundary(ring, base);
                        if(part.is_hole) holes.push_back(ring);
                        else { flush(); outer = ring; }
                    }
                    flush();
                }
                else if constexpr(std::is_same_v<T, artcc>)
                {
                    auto fs = styles.artcc_style(f.altitude);
                    line_style base = to_line_style(fs);
                    glm::vec4 fill_color{fs.r, fs.g, fs.b, 0.25F};
                    std::vector<glm::vec2> ring;
                    ring_to_mercator(f.points, ring);
                    emit_boundary(ring, base);
                    triangulate_polygon(ring, {}, fill_color, fill_out);
                }
                else if constexpr(std::is_same_v<T, adiz>)
                {
                    auto fs = styles.adiz_style();
                    line_style base = to_line_style(fs);
                    glm::vec4 fill_color{fs.r, fs.g, fs.b, 0.25F};
                    std::vector<glm::vec2> ring;
                    for(const auto& part : f.parts)
                    {
                        ring_to_mercator(part, ring);
                        emit_boundary(ring, base);
                        triangulate_polygon(ring, {}, fill_color, fill_out);
                    }
                }
                // Other variants (runway) handled in later stages.
            }, sel);
        }

        void build_vertices(const feature_build_request& req)
        {
            constexpr double WORLD_SIZE = 2.0 * HALF_CIRCUMFERENCE;

            // Request bbox is already padded by feature_renderer; use as-is.
            geo_bbox qbox{req.lon_min, req.lat_min, req.lon_max, req.lat_max};

            for(auto& p : poly) p.clear();
            world_labels.clear();

            // Normal query (includes handle_antimeridian copies at extended coords)
            build_all_features(qbox, req, 0.0);

            // If viewport extends past +180°, also query the [-180, 180] features
            // that appear on the right side, shifted by +world_size in Mercator
            if(qbox.lon_max > 180.0)
            {
                geo_bbox shifted{qbox.lon_min - 360.0, qbox.lat_min,
                                 qbox.lon_max - 360.0, qbox.lat_max};
                build_all_features(shifted, req, WORLD_SIZE);
            }

            // If viewport extends past -180°, query features shifted by -world_size
            if(qbox.lon_min < -180.0)
            {
                geo_bbox shifted{qbox.lon_min + 360.0, qbox.lat_min,
                                 qbox.lon_max + 360.0, qbox.lat_max};
                build_all_features(shifted, req, -WORLD_SIZE);
            }
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

        static bool sua_altitude_visible(const altitude_filter& af,
                                          int upper_ft, int lower_ft)
        {
            if(!af.any()) return false;
            if(lower_ft == 0 && upper_ft == 0) return true;   // unknown → visible
            return af.overlaps(lower_ft, upper_ft);
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
                                 const line_style& ls,
                                 double mx_offset)
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
                    static_cast<float>(lon_to_mx(pt.lon) + mx_offset),
                    static_cast<float>(lat_to_my(pt.lat)));
            }
            if(polyline.size() > 2 && polyline.front() != polyline.back())
            {
                polyline.push_back(polyline.front());
            }

            output.polylines.push_back(std::move(polyline));
            output.styles.push_back(ls);
        }

        void append_polyline(polyline_data& output,
                              const std::vector<airspace_point>& points,
                              const line_style& ls,
                              double mx_offset)
        {
            if(points.size() < 2)
            {
                return;
            }

            std::vector<glm::vec2> polyline;
            polyline.reserve(points.size());
            for(const auto& pt : points)
            {
                polyline.emplace_back(
                    static_cast<float>(lon_to_mx(pt.lon) + mx_offset),
                    static_cast<float>(lat_to_my(pt.lat)));
            }

            output.polylines.push_back(std::move(polyline));
            output.styles.push_back(ls);
        }

        // --- Feature build methods ---

        void emit_airport_icon(polyline_data& pd, float cx, float cy,
                                       float r, float pixels_per_world,
                                       const airport& apt, const feature_style& cs)
        {
            constexpr float APT_OUTER_SCALE = 1.2F;
            constexpr float APT_RING_WIDTH_PX = 1.0F;
            constexpr float APT_FILL_RADIUS = 0.5F;

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

        void build_airport_polylines(const geo_bbox& bbox,
                                      const feature_build_request& req,
                                      double mx_offset)
        {
            if(!styles.any_airport_visible(req.zoom)) return;
            const auto& airports = db.query_airports(bbox,
                styles.visible_airport_classes(req.zoom));

            float r = static_cast<float>(req.half_extent_y * SYMBOL_RADIUS_AIRPORT);
            float pixels_per_world = static_cast<float>(req.viewport_height / (2.0 * req.half_extent_y));

            for(const auto& apt : airports)
            {
                if(!styles.airport_visible(apt, req.zoom)) continue;

                const auto& cs = styles.airport_style(apt);
                float cx = static_cast<float>(lon_to_mx(apt.lon) + mx_offset);
                float cy = static_cast<float>(lat_to_my(apt.lat));

                emit_airport_icon(poly[layer_airports], cx, cy, r, pixels_per_world, apt, cs);

                // Label: prefer ICAO ID, fall back to FAA ID
                const auto& id = apt.icao_id.empty() ? apt.arpt_id : apt.icao_id;
                bool towered = apt.twr_type_code.find("ATCT") != std::string::npos;
                world_labels.push_back({id,
                    lon_to_mx(apt.lon) + mx_offset, lat_to_my(apt.lat),
                    towered ? LABEL_PRIORITY_AIRPORT_TOWERED : LABEL_PRIORITY_AIRPORT_UNTOWERED,
                    layer_airports});
            }
        }

        void emit_navaid_icon(polyline_data& pd, float cx, float cy, float r,
                               const navaid& nav, const feature_style& fs)
        {
            constexpr float NAV_NDB_CIRCLE = 0.4F;
            constexpr float NAV_DME_RECT = 0.85F;
            constexpr float NAV_VORDME_WIDTH = 1.1F;

            line_style ls = to_line_style(fs);
            line_style filled_ls = ls;
            filled_ls.fill_width = SYMBOL_FILL_PX;

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

        void build_navaid_polylines(const geo_bbox& bbox,
                                     const feature_build_request& req,
                                     double mx_offset)
        {
            if(!styles.any_navaid_visible(req.zoom)) return;
            if(!req.altitude.any()) return;
            constexpr float NAV_CLEARANCE = 2.0F;

            const auto& navaids = db.query_navaids(bbox);
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

                if(!styles.navaid_visible(nav.nav_type, req.zoom)) continue;

                bool keep = (nav.is_low && req.altitude.show_low)
                         || (nav.is_high && req.altitude.show_high);
                if(!keep) continue;

                const auto& fs = styles.navaid_style(nav.nav_type);

                float cx = static_cast<float>(lon_to_mx(nav.lon) + mx_offset);
                float cy = static_cast<float>(lat_to_my(nav.lat));

                navaid_positions.emplace_back(cx, cy);

                emit_navaid_icon(poly[layer_navaids], cx, cy, r, nav, fs);

                // Label: navaid ID
                bool is_vor = nav.nav_type.find("VOR") != std::string::npos
                    || nav.nav_type == "TACAN" || nav.nav_type == "VORTAC";
                world_labels.push_back({nav.nav_id,
                    lon_to_mx(nav.lon) + mx_offset, lat_to_my(nav.lat),
                    is_vor ? LABEL_PRIORITY_NAVAID_VOR : LABEL_PRIORITY_NAVAID_NDB,
                    layer_navaids});
            }
        }

        void build_airway_polylines(const geo_bbox& bbox,
                                     const feature_build_request& req,
                                     double mx_offset)
        {
            airway_waypoints.clear();
            if(!styles.any_airway_visible(req.zoom)) return;
            const auto& airways = db.query_airways(bbox);

            for(const auto& seg : airways)
            {
                if(!styles.airway_visible(seg.awy_id, req.zoom))
                {
                    continue;
                }

                if(!altitude_filter_allows(req.altitude, airway_bands(seg.awy_id))) continue;

                airway_waypoints.insert(seg.from_point);
                airway_waypoints.insert(seg.to_point);

                const auto& fs = styles.airway_style(seg.awy_id);

                float x0 = static_cast<float>(lon_to_mx(seg.from_lon) + mx_offset);
                float y0 = static_cast<float>(lat_to_my(seg.from_lat));
                float x1 = static_cast<float>(lon_to_mx(seg.to_lon) + mx_offset);
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

        void emit_fix_icon(polyline_data& pd, float cx, float cy, float r,
                           const fix& f, const feature_style& fs)
        {
            line_style ls = to_line_style(fs);
            ls.fill_width = SYMBOL_FILL_PX;
            if(f.use_code == "RP" || f.use_code == "MR")
            {
                add_triangle_polyline(pd, cx, cy, r, ls);
            }
            else
            {
                add_waypoint_star_polyline(pd, cx, cy, r, ls);
            }
        }

        void build_fix_polylines(const geo_bbox& bbox,
                                  const feature_build_request& req,
                                  double mx_offset)
        {
            if(!styles.any_fix_visible(req.zoom)) return;
            if(!req.altitude.any()) return;
            const auto& fixes = db.query_fixes(bbox);
            float radius = static_cast<float>(req.half_extent_y * SYMBOL_RADIUS_FIX);

            for(const auto& fix : fixes)
            {
                if(!styles.fix_visible(fix_on_airway(fix.fix_id), req.zoom)) continue;

                bool keep = (fix.is_low && req.altitude.show_low)
                         || (fix.is_high && req.altitude.show_high);
                if(!keep) continue;

                const auto& fs = styles.fix_style(fix.use_code);

                float cx = static_cast<float>(lon_to_mx(fix.lon) + mx_offset);
                float cy = static_cast<float>(lat_to_my(fix.lat));

                emit_fix_icon(poly[layer_fixes], cx, cy, radius, fix, fs);

                // Label: fix ID
                bool on_airway = fix_on_airway(fix.fix_id);
                world_labels.push_back({fix.fix_id,
                    lon_to_mx(fix.lon) + mx_offset, lat_to_my(fix.lat),
                    on_airway ? LABEL_PRIORITY_FIX_AIRWAY : LABEL_PRIORITY_FIX_OTHER,
                    layer_fixes});
            }
        }

        void build_mtr_polylines(const geo_bbox& bbox,
                                  const feature_build_request& req,
                                  double mx_offset)
        {
            if(!styles.mtr_visible(req.zoom))
            {
                return;
            }
            if(!req.altitude.any()) return;

            const auto& mtrs = db.query_mtrs(bbox);
            const auto& fs = styles.mtr_style();

            for(const auto& seg : mtrs)
            {
                if(!altitude_filter_allows(req.altitude, mtr_bands(seg.route_type_code))) continue;

                float x0 = static_cast<float>(lon_to_mx(seg.from_lon) + mx_offset);
                float y0 = static_cast<float>(lat_to_my(seg.from_lat));
                float x1 = static_cast<float>(lon_to_mx(seg.to_lon) + mx_offset);
                float y1 = static_cast<float>(lat_to_my(seg.to_lat));

                poly[layer_mtrs].polylines.push_back({glm::vec2(x0, y0), glm::vec2(x1, y1)});
                poly[layer_mtrs].styles.push_back(to_line_style(fs));
            }
        }

        void build_runway_polylines(const geo_bbox& bbox,
                                     const feature_build_request& req,
                                     double mx_offset)
        {
            if(!styles.runway_visible(req.zoom))
            {
                return;
            }
            if(!req.altitude.low_enabled()) return;

            const auto& runways = db.query_runways(bbox);
            const auto& fs = styles.runway_style();

            for(const auto& rwy : runways)
            {
                float x0 = static_cast<float>(lon_to_mx(rwy.end1_lon) + mx_offset);
                float y0 = static_cast<float>(lat_to_my(rwy.end1_lat));
                float x1 = static_cast<float>(lon_to_mx(rwy.end2_lon) + mx_offset);
                float y1 = static_cast<float>(lat_to_my(rwy.end2_lat));

                poly[layer_runways].polylines.push_back({glm::vec2(x0, y0), glm::vec2(x1, y1)});
                poly[layer_runways].styles.push_back(to_line_style(fs));
            }
        }

        void build_sua_polylines(const geo_bbox& bbox,
                                  const feature_build_request& req,
                                  double mx_offset)
        {
            if(!styles.any_sua_visible(req.zoom)) return;
            if(!req.altitude.any()) return;
            auto sua_filter = styles.visible_sua_types(req.zoom);
            // Polygon rings from subdivided segments
            const auto& segs = db.query_sua_segments(bbox, sua_filter);

            for(const auto& seg : segs)
            {
                if(!styles.sua_visible(seg.sua_type, req.zoom)) continue;
                if(!sua_altitude_visible(req.altitude, seg.upper_ft_msl, seg.lower_ft_msl)) continue;
                auto ls = to_line_style(styles.sua_style(seg.sua_type));

                append_polyline(poly[layer_sua], seg.points, ls, mx_offset);
            }

            // Circles: narrow render-only query (single JOIN'd statement,
            // no N+1 per-row shape/circle lookups).
            const auto& circles = db.query_sua_circles(bbox, sua_filter);
            for(const auto& c : circles)
            {
                if(!sua_altitude_visible(req.altitude, c.upper_ft_msl, c.lower_ft_msl)) continue;
                auto ls = to_line_style(styles.sua_style(c.sua_type));
                double lat_rad = c.center_lat * M_PI / 180.0;
                float cx = static_cast<float>(lon_to_mx(c.center_lon) + mx_offset);
                float cy = static_cast<float>(lat_to_my(c.center_lat));
                float r = static_cast<float>(c.radius_nm * 1852.0 / std::cos(lat_rad));
                poly[layer_sua].circles.push_back({{cx, cy}, r, ls});
            }
        }

        void emit_pja_point_icon(polyline_data& pd, float cx, float cy, float r,
                                  const feature_style& fs)
        {
            auto ls = to_line_style(fs);
            ls.fill_width = SYMBOL_FILL_PX;
            pd.polylines.push_back({
                {cx + r, cy}, {cx, cy + r}, {cx - r, cy},
                {cx, cy - r}, {cx + r, cy},
            });
            pd.styles.push_back(ls);

            float lh = r * LETTER_HEIGHT;
            float lw = lh * LETTER_ASPECT;
            line_style white_ls = {LETTER_WIDTH_PX, 0, 0, 0, 1, 1, 1, 1, 0};
            draw_letter(pd, letter_P, cx, cy, lw, lh, white_ls);
        }

        void emit_maa_point_icon(polyline_data& pd, float cx, float cy, float r,
                                  const maa& m, const feature_style& fs)
        {
            auto ls = to_line_style(fs);
            ls.fill_width = SYMBOL_FILL_PX;
            pd.polylines.push_back({
                {cx + r, cy}, {cx, cy + r}, {cx - r, cy},
                {cx, cy - r}, {cx + r, cy},
            });
            pd.styles.push_back(ls);

            const letter_def* ld = maa_letter(m.type);
            if(ld)
            {
                float lh = r * LETTER_HEIGHT;
                float lw = lh * LETTER_ASPECT;
                line_style white_ls = {LETTER_WIDTH_PX, 0, 0, 0, 1, 1, 1, 1, 0};
                draw_letter(pd, *ld, cx, cy, lw, lh, white_ls);
            }
        }

        void build_pja_polylines(const geo_bbox& bbox,
                                  const feature_build_request& req,
                                  double mx_offset)
        {
            bool area_vis = styles.pja_area_visible(req.zoom);
            bool point_vis = styles.pja_point_visible(req.zoom);
            if(!area_vis && !point_vis) return;
            if(!req.altitude.any()) return;

            constexpr double NM_TO_METERS = 1852.0;
            constexpr double SYMBOL_RADIUS_PJA = SYMBOL_RADIUS_AIRPORT;

            const auto& pjas = db.query_pjas(bbox);

            for(const auto& p : pjas)
            {
                int upper = p.max_altitude_ft_msl > 0 ? p.max_altitude_ft_msl
                                                      : altitude_filter::UNLIMITED_FT;
                if(!req.altitude.overlaps(0, upper)) continue;

                if(p.radius_nm > 0.0 && area_vis)
                {
                    double lat_rad = p.lat * M_PI / 180.0;
                    float cx = static_cast<float>(lon_to_mx(p.lon) + mx_offset);
                    float cy = static_cast<float>(lat_to_my(p.lat));
                    float r = static_cast<float>(p.radius_nm * NM_TO_METERS / std::cos(lat_rad));

                    auto ls = to_line_style(styles.pja_area_style());
                    poly[layer_pja].circles.push_back({{cx, cy}, r, ls});
                }
                else if(p.radius_nm <= 0.0 && point_vis)
                {
                    float cx = static_cast<float>(lon_to_mx(p.lon) + mx_offset);
                    float cy = static_cast<float>(lat_to_my(p.lat));
                    float r = static_cast<float>(req.half_extent_y * SYMBOL_RADIUS_PJA);
                    emit_pja_point_icon(poly[layer_pja], cx, cy, r, styles.pja_point_style());
                }
            }
        }

        void build_maa_polylines(const geo_bbox& bbox,
                                  const feature_build_request& req,
                                  double mx_offset)
        {
            bool area_vis = styles.maa_area_visible(req.zoom);
            bool point_vis = styles.maa_point_visible(req.zoom);
            if(!area_vis && !point_vis) return;
            if(!req.altitude.low_enabled()) return;

            constexpr double NM_TO_METERS = 1852.0;

            const auto& maas = db.query_maas(bbox);

            for(const auto& m : maas)
            {
                if(!m.shape.empty() && area_vis)
                {
                    auto ls = to_line_style(styles.maa_area_style());
                    append_polygon_ring(poly[layer_maa], m.shape, ls, mx_offset);
                }
                else if(m.radius_nm > 0.0 && area_vis)
                {
                    double lat_rad = m.lat * M_PI / 180.0;
                    float cx = static_cast<float>(lon_to_mx(m.lon) + mx_offset);
                    float cy = static_cast<float>(lat_to_my(m.lat));
                    float r = static_cast<float>(m.radius_nm * NM_TO_METERS / std::cos(lat_rad));

                    auto ls = to_line_style(styles.maa_area_style());
                    poly[layer_maa].circles.push_back({{cx, cy}, r, ls});
                }
                else if(m.lat != 0.0 && point_vis)
                {
                    float cx = static_cast<float>(lon_to_mx(m.lon) + mx_offset);
                    float cy = static_cast<float>(lat_to_my(m.lat));
                    float r = static_cast<float>(req.half_extent_y * SYMBOL_RADIUS_AIRPORT);
                    emit_maa_point_icon(poly[layer_maa], cx, cy, r, m, styles.maa_point_style());
                }
            }
        }

        void build_adiz_polylines(const geo_bbox& bbox,
                                   const feature_build_request& req,
                                   double mx_offset)
        {
            if(!styles.adiz_visible(req.zoom)) return;

            const auto& segs = db.query_adiz_segments(
                bbox);
            auto ls = to_line_style(styles.adiz_style());

            for(const auto& seg : segs)
            {
                append_polyline(poly[layer_adiz], seg.points, ls, mx_offset);
            }
        }

        void build_artcc_polylines(const geo_bbox& bbox,
                                    const feature_build_request& req,
                                    double mx_offset)
        {
            if(!styles.any_artcc_visible(req.zoom)) return;
            if(!req.altitude.any()) return;
            const auto& segs = db.query_artcc_segments(
                bbox);

            for(const auto& seg : segs)
            {
                if(!styles.artcc_visible(seg.altitude, req.zoom)) continue;
                if(!altitude_filter_allows(req.altitude, artcc_bands(seg.altitude))) continue;

                auto ls = to_line_style(styles.artcc_style(seg.altitude));

                append_polyline(poly[layer_artcc], seg.points, ls, mx_offset);
            }
        }

        void emit_obstacle_icon(polyline_data& pd, float cx, float cy, float r,
                                const obstacle& obs, const feature_style& fs)
        {
            auto ls = to_line_style(fs);
            bool lighted = obs.lighting != "N";
            add_obstacle_polylines(pd, cx, cy, r, obs.agl_ht >= 1000, lighted, ls);
        }

        void build_obstacle_polylines(const geo_bbox& bbox,
                                       const feature_build_request& req,
                                       double mx_offset)
        {
            if(!styles.any_obstacle_visible(req.zoom)) return;
            if(!req.altitude.low_enabled()) return;
            const auto& obstacles = db.query_obstacles(bbox);
            float radius = static_cast<float>(req.half_extent_y * SYMBOL_RADIUS_OBSTACLE);

            for(const auto& obs : obstacles)
            {
                if(!styles.obstacle_visible(obs.agl_ht, req.zoom)) continue;

                const auto& fs = styles.obstacle_style(obs.agl_ht);
                float cx = static_cast<float>(lon_to_mx(obs.lon) + mx_offset);
                float cy = static_cast<float>(lat_to_my(obs.lat));
                emit_obstacle_icon(poly[layer_obstacles], cx, cy, radius, obs, fs);
            }
        }

        void build_airspace_polylines(const geo_bbox& bbox,
                                       const feature_build_request& req,
                                       double mx_offset)
        {
            if(!styles.any_airspace_visible(req.zoom)) return;
            if(!req.altitude.low_enabled()) return;
            const auto& segs = db.query_class_airspace_segments(bbox,
                styles.visible_airspace_values(req.zoom));

            for(const auto& seg : segs)
            {
                if(!styles.airspace_visible(seg.airspace_class, seg.local_type, req.zoom)) continue;
                auto ls = to_line_style(styles.airspace_style(seg.airspace_class, seg.local_type));

                append_polyline(poly[layer_airspace], seg.points, ls, mx_offset);
            }
        }

        void emit_comm_icon(polyline_data& pd, float cx, float cy, float r,
                             const line_style& ls)
        {
            add_comm_symbol(pd, cx, cy, r * 0.75F, ls);
        }

        template<typename T>
        void build_comm_polylines(const std::vector<T>& features,
                                   int layer_id, const line_style& ls,
                                   const feature_build_request& req,
                                   double mx_offset)
        {
            float radius = static_cast<float>(req.half_extent_y * SYMBOL_RADIUS_COMM);
            for(const auto& f : features)
            {
                float cx = static_cast<float>(lon_to_mx(f.lon) + mx_offset);
                float cy = static_cast<float>(lat_to_my(f.lat));
                emit_comm_icon(poly[layer_id], cx, cy, radius, ls);
            }
        }

        void build_rco_polylines(const geo_bbox& bbox,
                                  const feature_build_request& req,
                                  double mx_offset)
        {
            if(!styles.rco_visible(req.zoom)) return;
            if(!req.altitude.low_enabled()) return;
            build_comm_polylines(db.query_comm_outlets(bbox),
                                 layer_rco, to_line_style(styles.rco_style()), req, mx_offset);
        }

        void build_awos_polylines(const geo_bbox& bbox,
                                   const feature_build_request& req,
                                   double mx_offset)
        {
            if(!styles.awos_visible(req.zoom)) return;
            if(!req.altitude.low_enabled()) return;
            build_comm_polylines(db.query_awos(bbox),
                                 layer_awos, to_line_style(styles.awos_style()), req, mx_offset);
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
