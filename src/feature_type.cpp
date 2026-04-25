#include "feature_type.hpp"

#include "altitude_filter.hpp"
#include "chart_style.hpp"
#include "geo_math.hpp"
#include "map_view.hpp"
#include "nasr_database.hpp"
#include "ui_overlay.hpp"  // for the layer enum

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <glm/glm.hpp>
#include <mapbox/earcut.hpp>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <variant>

namespace mapbox {
namespace util {
    template <> struct nth<0, glm::vec2> { static float get(const glm::vec2& p) { return p.x; } };
    template <> struct nth<1, glm::vec2> { static float get(const glm::vec2& p) { return p.y; } };
}}

namespace osect
{
    namespace
    {
        // -- Dimensional constants ---------------------------------------------

        constexpr auto SYMBOL_RADIUS_AIRPORT  = 0.012;
        constexpr auto SYMBOL_RADIUS_FIX      = 0.012;
        constexpr auto SYMBOL_RADIUS_OBSTACLE = 0.012;
        constexpr auto SYMBOL_RADIUS_COMM     = 0.012;

        constexpr auto LETTER_HEIGHT   = 0.385;
        constexpr auto LETTER_ASPECT   = 0.7;
        constexpr auto LETTER_WIDTH_PX = 2.0F;
        constexpr auto SYMBOL_FILL_PX  = 50.0F;

        // -- Polyline geometry helpers -----------------------------------------

        void triangulate_polygon(
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
            auto indices = mapbox::earcut<uint32_t>(rings);
            std::vector<glm::vec2> flat;
            flat.reserve(outer.size() + [&] {
                size_t s = 0; for(const auto& h : holes) s += h.size(); return s;
            }());
            flat.insert(flat.end(), outer.begin(), outer.end());
            for(const auto& h : holes) flat.insert(flat.end(), h.begin(), h.end());
            out.triangles.reserve(out.triangles.size() + indices.size());
            for(uint32_t idx : indices)
                out.triangles.push_back({flat[idx], color});
        }

        void add_circle(polyline_data& pd, double cx, double cy, double r,
                        const line_style& ls)
        {
            const auto n = 16;
            std::vector<glm::vec2> pts;
            for(int i = 0; i < n; i++)
            {
                auto angle = 2.0 * M_PI * i / n;
                pts.emplace_back(static_cast<float>(cx + r * std::cos(angle)),
                                 static_cast<float>(cy + r * std::sin(angle)));
            }
            pts.push_back(pts.front());
            pd.polylines.push_back(std::move(pts));
            pd.styles.push_back(ls);
        }

        void add_circle_to(polyline_data& pd, double cx, double cy, double r,
                           const line_style& ls, int n = 24)
        {
            std::vector<glm::vec2> pts;
            for(int i = 0; i < n; i++)
            {
                auto angle = 2.0 * M_PI * i / n;
                pts.emplace_back(static_cast<float>(cx + r * std::cos(angle)),
                                 static_cast<float>(cy + r * std::sin(angle)));
            }
            pts.push_back(pts.front());
            pd.polylines.push_back(std::move(pts));
            pd.styles.push_back(ls);
        }

        void add_seg_to(polyline_data& pd, double x0, double y0,
                        double x1, double y1, const line_style& ls)
        {
            pd.polylines.push_back({
                glm::vec2(static_cast<float>(x0), static_cast<float>(y0)),
                glm::vec2(static_cast<float>(x1), static_cast<float>(y1))});
            pd.styles.push_back(ls);
        }

        void add_hexagon(polyline_data& pd, double cx, double cy, double r,
                         const line_style& ls)
        {
            std::vector<glm::vec2> pts;
            for(int i = 0; i < 6; i++)
            {
                auto angle = glm::radians(60.0 * i);
                pts.emplace_back(static_cast<float>(cx + r * std::cos(angle)),
                                 static_cast<float>(cy + r * std::sin(angle)));
            }
            pts.push_back(pts.front());
            pd.polylines.push_back(std::move(pts));
            pd.styles.push_back(ls);
        }

        void add_rect(polyline_data& pd, double cx, double cy, double hw, double hh,
                      const line_style& ls)
        {
            auto l = static_cast<float>(cx - hw);
            auto r = static_cast<float>(cx + hw);
            auto t = static_cast<float>(cy - hh);
            auto b = static_cast<float>(cy + hh);
            pd.polylines.push_back({
                {l, t}, {r, t}, {r, b}, {l, b}, {l, t}
            });
            pd.styles.push_back(ls);
        }

        void add_center_dot(polyline_data& pd, double cx, double cy, double r,
                            const line_style& ls)
        {
            auto d = r * 0.15;
            auto fcx = static_cast<float>(cx);
            auto fcy = static_cast<float>(cy);
            auto left = static_cast<float>(cx - d);
            auto right = static_cast<float>(cx + d);
            auto up = static_cast<float>(cy + d);
            auto down = static_cast<float>(cy - d);
            pd.polylines.push_back({
                {left, fcy}, {fcx, up}, {right, fcy}, {fcx, down}, {left, fcy}
            });
            pd.styles.push_back(ls);
        }

        void add_caltrop(polyline_data& pd, double cx, double cy, double hex_r,
                         const line_style& ls)
        {
            // Three blades with three-fold rotational symmetry. Each blade is
            // a trapezoid whose inner edge runs along one hex edge of radius
            // hex_r and whose outer edge is offset by h along that edge's
            // outward normal.
            const auto h = hex_r * 0.5;
            for(int i = 0; i < 3; i++)
            {
                const auto base_deg = 120.0 * i;
                const auto va_x = cx + hex_r * std::cos(glm::radians(base_deg));
                const auto va_y = cy + hex_r * std::sin(glm::radians(base_deg));
                const auto vb_x = cx + hex_r * std::cos(glm::radians(base_deg + 60.0));
                const auto vb_y = cy + hex_r * std::sin(glm::radians(base_deg + 60.0));
                const auto nx = h * std::cos(glm::radians(base_deg + 30.0));
                const auto ny = h * std::sin(glm::radians(base_deg + 30.0));
                const glm::vec2 va{static_cast<float>(va_x), static_cast<float>(va_y)};
                const glm::vec2 vb{static_cast<float>(vb_x), static_cast<float>(vb_y)};
                const glm::vec2 van{static_cast<float>(va_x + nx), static_cast<float>(va_y + ny)};
                const glm::vec2 vbn{static_cast<float>(vb_x + nx), static_cast<float>(vb_y + ny)};
                pd.polylines.push_back({va, vb, vbn, van, va});
                pd.styles.push_back(ls);
            }
        }

        void add_comm_symbol(polyline_data& pd, double cx, double cy,
                              double radius, const line_style& ls)
        {
            add_circle_to(pd, cx, cy, radius, ls);
            auto dot_r = radius * 0.2;
            add_circle_to(pd, cx, cy, dot_r, ls, 8);
        }

        // Project a sequence of lat/lon points to Mercator and append
        // as a polyline. Long edges are subdivided along the great circle.
        void add_polyline(polyline_data& pd,
                          const std::vector<airspace_point>& points,
                          const line_style& ls,
                          double mx_offset)
        {
            if(points.size() < 2) return;
            std::vector<glm::vec2> polyline;
            polyline.reserve(points.size());
            polyline.emplace_back(
                static_cast<float>(lon_to_mx(points[0].lon) + mx_offset),
                static_cast<float>(lat_to_my(points[0].lat)));
            for(std::size_t i = 1; i < points.size(); i++)
            {
                auto seg = geodesic_interpolate(
                    points[i - 1].lat, points[i - 1].lon,
                    points[i].lat, points[i].lon);
                for(std::size_t j = 1; j < seg.size(); j++)
                    polyline.emplace_back(
                        static_cast<float>(lon_to_mx(seg[j].lon) + mx_offset),
                        static_cast<float>(lat_to_my(seg[j].lat)));
            }
            pd.polylines.push_back(std::move(polyline));
            pd.styles.push_back(ls);
        }

        // Generate a geodesic circle as a Mercator polyline.
        void add_geodesic_circle(polyline_data& pd,
                                  double center_lat, double center_lon,
                                  double radius_nm,
                                  const line_style& ls,
                                  double mx_offset)
        {
            auto pts = geodesic_circle(center_lat, center_lon, radius_nm);
            std::vector<glm::vec2> polyline;
            polyline.reserve(pts.size());
            for(const auto& p : pts)
                polyline.emplace_back(
                    static_cast<float>(lon_to_mx(p.lon) + mx_offset),
                    static_cast<float>(lat_to_my(p.lat)));
            pd.polylines.push_back(std::move(polyline));
            pd.styles.push_back(ls);
        }

        // --- Vector letters ---
        // clang-format off
        const std::vector<float> letter_H = {-1,-1, -1,1,  1,-1, 1,1,  -1,.1f, 1,.1f};
        const std::vector<float> letter_F = {-1,-1, -1,1,  -1,1, 1,1,  -1,.1f, .6f,.1f};
        const std::vector<float> letter_G = {1,1, -1,1,  -1,1, -1,-1,  -1,-1, 1,-1,  1,-1, 1,.1f,  1,.1f, 0,.1f};
        const std::vector<float> letter_S = {1,1, -1,1,  -1,1, -1,.1f,  -1,.1f, 1,.1f,  1,.1f, 1,-1,  1,-1, -1,-1};
        const std::vector<float> letter_B = {-1,-1, -1,1,  -1,1, 1,1,  1,1, 1,.1f,  1,.1f, -1,.1f,  -1,-1, 1,-1,  1,-1, 1,.1f};
        const std::vector<float> letter_X = {-1,-1, 1,1,  -1,1, 1,-1};
        const std::vector<float> letter_M = {-1,-1, -1,1,  -1,1, 0,.1f,  0,.1f, 1,1,  1,1, 1,-1};
        const std::vector<float> letter_R = {-1,-1, -1,1,  -1,1, 1,1,  1,1, 1,.1f,  1,.1f, -1,.1f,  0,.1f, 1,-1};
        const std::vector<float> letter_P = {-1,-1, -1,1,  -1,1, 1,1,  1,1, 1,.1f,  1,.1f, -1,.1f};
        const std::vector<float> letter_A = {-1,-1, 0,1,  0,1, 1,-1,  -0.5F,.1f, 0.5F,.1f};
        const std::vector<float> letter_U = {-1,1, -1,-1,  -1,-1, 1,-1,  1,-1, 1,1};
        // clang-format on

        void add_letter(polyline_data& pd, const std::vector<float>& segs,
                         double cx, double cy, double w, double h,
                         const line_style& ls)
        {
            for(size_t i = 0; i + 3 < segs.size(); i += 4)
            {
                auto x0 = cx + segs[i + 0] * w;
                auto y0 = cy + segs[i + 1] * h;
                auto x1 = cx + segs[i + 2] * w;
                auto y1 = cy + segs[i + 3] * h;
                add_seg_to(pd, x0, y0, x1, y1, ls);
            }
        }

        // -- Geometry picking helpers -------------------------------------------

        bool point_in_ring(double px, double py,
                        const std::vector<airspace_point>& ring)
        {
            auto inside = false;
            for(size_t i = 0, j = ring.size() - 1; i < ring.size(); j = i++)
            {
                auto yi = ring[i].lat;
                auto yj = ring[j].lat;
                auto xi = ring[i].lon;
                auto xj = ring[j].lon;
                if(((yi > py) != (yj > py)) &&
                (px < (xj - xi) * (py - yi) / (yj - yi) + xi))
                {
                    inside = !inside;
                }
            }
            return inside;
        }

        bool point_in_multi_ring(double lon, double lat,
                                const std::vector<polygon_ring>& rings)
        {
            auto inside = false;
            for(const auto& ring : rings)
            {
                if(point_in_ring(lon, lat, ring.points))
                {
                    if(ring.is_hole) { inside = false; break; }
                    inside = true;
                }
            }
            return inside;
        }

        bool point_in_circle_nm(double px, double py,
                                double cx, double cy, double radius_nm)
        {
            auto dlat = (py - cy) * 60.0;
            auto dlon = (px - cx) * 60.0 * std::cos(cy * M_PI / 180.0);
            return (dlat * dlat + dlon * dlon) <= (radius_nm * radius_nm);
        }

        double point_to_segment_nm(double px, double py,
                                    double ax, double ay, double bx, double by)
        {
            auto cos_lat = std::cos(py * M_PI / 180.0);
            auto ax_nm = (ax - px) * 60.0 * cos_lat;
            auto ay_nm = (ay - py) * 60.0;
            auto bx_nm = (bx - px) * 60.0 * cos_lat;
            auto by_nm = (by - py) * 60.0;

            auto dx = bx_nm - ax_nm;
            auto dy = by_nm - ay_nm;
            auto len2 = dx * dx + dy * dy;
            double t = 0;
            if(len2 > 0)
            {
                t = -(ax_nm * dx + ay_nm * dy) / len2;
                if(t < 0) t = 0;
                else if(t > 1) t = 1;
            }
            auto cx = ax_nm + t * dx;
            auto cy = ay_nm + t * dy;
            return std::sqrt(cx * cx + cy * cy);
        }

        line_style to_line_style(const feature_style& fs)
        {
            return {fs.line_width, fs.border_width, fs.dash_length, fs.gap_length,
                    fs.r, fs.g, fs.b, fs.a, 0};
        }

        // True if a SUA stratum's altitude bounds pass the active filter.
        // Missing bounds (all zero + empty refs) are treated as visible.
        bool sua_altitude_visible(const altitude_filter& af,
                                   int upper_ft, const std::string& upper_ref,
                                   int lower_ft, const std::string& lower_ref)
        {
            if(!af.any()) return false;
            if(lower_ft == 0 && upper_ft == 0 && lower_ref.empty() && upper_ref.empty())
                return true;
            return af.overlaps(lower_ft, lower_ref, upper_ft, upper_ref);
        }

        // -- Formatting helpers for info_kv -------------------------------------

        std::string fmt_int(int v)
        {
            std::ostringstream os;
            os << v;
            return os.str();
        }

        std::string fmt_dbl(double v, int prec)
        {
            std::ostringstream os;
            os.setf(std::ios::fixed);
            os.precision(prec);
            os << v;
            return os.str();
        }

        // -- Concrete feature types ---------------------------------------------

        // Helper: feature_types whose feature exposes .lon/.lat fields
        // (airports, navaids, fixes, obstacles, comm outlets, AWOS) all
        // implement point_coord identically. Defined as a free template
        // so each layer's override is a one-line forward.
        template<typename T>
        std::optional<std::pair<double, double>> coord_of(const feature& f)
        {
            const auto& v = std::get<T>(f);
            return std::make_pair(v.lon, v.lat);
        }

        // Common metadata + std::holds_alternative dispatch. The `FType`
        // macro argument is the feature variant alternative this
        // feature_type owns. POINT types additionally override point_coord()
        // to return their feature's intrinsic lon/lat; AREA types leave it
        // returning nullopt and the default anchor_lonlat() falls back to
        // the click point.
        #define FEATURE_TYPE_BODY(LBL, ID, TAG, FType) \
            using feature_t = FType; \
            const char* label() const override { return LBL; } \
            int layer_id() const override { return ID; } \
            const char* section_tag() const override { return TAG; } \
            bool owns(const feature& f) const override { return std::holds_alternative<feature_t>(f); } \
            void pick(const pick_context&, std::vector<feature>&) const override; \
            std::string summary(const feature& f) const override; \
            kv_list info_kv(const feature& f) const override; \
            void build(const build_context& ctx) const override; \
            void build_selection(const build_context& ctx, const feature& f, \
                                  polyline_data& out, polygon_fill_data& fill_out) const override;

        #define DECLARE_AREA_FEATURE_TYPE(Class, LBL, ID, TAG, FType) \
            class Class : public feature_type { \
            public: \
                FEATURE_TYPE_BODY(LBL, ID, TAG, FType) \
            };

        #define DECLARE_POINT_FEATURE_TYPE(Class, LBL, ID, TAG, FType) \
            class Class : public feature_type { \
            public: \
                FEATURE_TYPE_BODY(LBL, ID, TAG, FType) \
                std::optional<std::pair<double,double>> \
                point_coord(const feature& f) const override \
                { return coord_of<feature_t>(f); } \
            };

        DECLARE_POINT_FEATURE_TYPE(airport_type,     "Airports",  layer_airports,  "APT",   airport)
        DECLARE_AREA_FEATURE_TYPE (runway_type,      "Runways",   layer_runways,   "RWY",   runway)
        DECLARE_POINT_FEATURE_TYPE(navaid_type,      "Navaids",   layer_navaids,   "NAV",   navaid)
        DECLARE_POINT_FEATURE_TYPE(fix_type,         "Fixes",     layer_fixes,     "FIX",   fix)
        DECLARE_AREA_FEATURE_TYPE (airway_type,      "Airways",   layer_airways,   "AWY",   airway_segment)
        DECLARE_AREA_FEATURE_TYPE (mtr_type,         "MTRs",      layer_mtrs,      "MTR",   mtr_segment)
        DECLARE_AREA_FEATURE_TYPE (pja_type,         "PJA",       layer_pja,       "PJA",   pja)
        DECLARE_AREA_FEATURE_TYPE (maa_type,         "MAA",       layer_maa,       "MAA",   maa)
        DECLARE_AREA_FEATURE_TYPE (airspace_type,    "Airspace",  layer_airspace,  "CLS",   class_airspace)
        DECLARE_AREA_FEATURE_TYPE (sua_type,         "SUA",       layer_sua,       "SUA",   sua)
        DECLARE_AREA_FEATURE_TYPE (tfr_type,         "TFRs",      layer_tfr,       "TFR",   tfr)
        DECLARE_AREA_FEATURE_TYPE (adiz_type,        "ADIZ",      layer_adiz,      "ADIZ",  adiz)
        DECLARE_AREA_FEATURE_TYPE (artcc_type,       "ARTCC",     layer_artcc,     "ARTCC", artcc)
        DECLARE_POINT_FEATURE_TYPE(obstacle_type,    "Obstacles", layer_obstacles, "OBS",   obstacle)
        DECLARE_POINT_FEATURE_TYPE(comm_outlet_type, "RCO",       layer_rco,       "COM",   comm_outlet)
        DECLARE_POINT_FEATURE_TYPE(awos_type,        "AWOS",      layer_awos,      "AWOS",  awos)

        #undef DECLARE_AREA_FEATURE_TYPE
        #undef DECLARE_POINT_FEATURE_TYPE
        #undef FEATURE_TYPE_BODY

        // -- pick() bodies ---------------------------------------------------

        void airport_type::pick(const pick_context& ctx,
                                  std::vector<feature>& out) const
        {
            for(const auto& apt : ctx.db.query_airports(ctx.pick_box))
                if(ctx.styles.airport_visible(apt, ctx.zoom))
                    out.push_back(apt);
        }

        void navaid_type::pick(const pick_context& ctx,
                                 std::vector<feature>& out) const
        {
            if(!ctx.vis.altitude.any()) return;
            for(const auto& nav : ctx.db.query_navaids(ctx.pick_box))
            {
                if(!ctx.styles.navaid_visible(nav.nav_type, ctx.zoom)) continue;
                auto keep = (nav.is_low && ctx.vis.altitude.show_low)
                         || (nav.is_high && ctx.vis.altitude.show_high);
                if(keep) out.push_back(nav);
            }
        }

        void fix_type::pick(const pick_context& ctx,
                              std::vector<feature>& out) const
        {
            if(!ctx.vis.altitude.any()) return;
            if(!ctx.styles.fix_visible(true, ctx.zoom) &&
               !ctx.styles.fix_visible(false, ctx.zoom)) return;
            for(const auto& f : ctx.db.query_fixes(ctx.pick_box))
            {
                auto keep = (f.is_low && ctx.vis.altitude.show_low)
                         || (f.is_high && ctx.vis.altitude.show_high);
                if(keep) out.push_back(f);
            }
        }

        void obstacle_type::pick(const pick_context& ctx,
                                   std::vector<feature>& out) const
        {
            if(!ctx.vis.altitude.low_enabled()) return;
            for(const auto& obs : ctx.db.query_obstacles(ctx.pick_box))
                if(ctx.styles.obstacle_visible(obs.agl_ht, ctx.zoom))
                    out.push_back(obs);
        }

        void comm_outlet_type::pick(const pick_context& ctx,
                                      std::vector<feature>& out) const
        {
            if(!ctx.vis.altitude.low_enabled() ||
               !ctx.styles.rco_visible(ctx.zoom)) return;
            for(const auto& c : ctx.db.query_comm_outlets(ctx.pick_box))
                out.push_back(c);
        }

        void awos_type::pick(const pick_context& ctx,
                               std::vector<feature>& out) const
        {
            if(!ctx.vis.altitude.low_enabled() ||
               !ctx.styles.awos_visible(ctx.zoom)) return;
            for(const auto& a : ctx.db.query_awos(ctx.pick_box))
                out.push_back(a);
        }

        void airspace_type::pick(const pick_context& ctx,
                                         std::vector<feature>& out) const
        {
            if(!ctx.vis.altitude.low_enabled()) return;
            for(const auto& a : ctx.db.query_class_airspace(ctx.click_box))
            {
                if(!ctx.styles.airspace_visible(a.airspace_class, a.local_type, ctx.zoom))
                    continue;
                if(point_in_multi_ring(ctx.click_lon, ctx.click_lat, a.parts))
                    out.push_back(a);
            }
        }

        void sua_type::pick(const pick_context& ctx,
                              std::vector<feature>& out) const
        {
            if(!ctx.vis.altitude.any()) return;
            for(const auto& s : ctx.db.query_sua(ctx.click_box))
            {
                if(!ctx.styles.sua_visible(s.sua_type, ctx.zoom)) continue;
                for(const auto& stratum : s.strata)
                {
                    if(!ctx.vis.altitude.overlaps(stratum.lower_ft_val, stratum.lower_ft_ref,
                                                   stratum.upper_ft_val, stratum.upper_ft_ref))
                        continue;
                    auto inside = false;
                    for(const auto& ring : stratum.parts)
                    {
                        if(point_in_ring(ctx.click_lon, ctx.click_lat, ring.points))
                        {
                            if(ring.is_hole) { inside = false; break; }
                            inside = true;
                        }
                    }
                    if(!inside) continue;
                    sua pick = s;
                    pick.strata.clear();
                    pick.strata.push_back(stratum);
                    pick.upper_limit = stratum.upper_limit;
                    pick.lower_limit = stratum.lower_limit;
                    out.push_back(std::move(pick));
                }
            }
        }

        void artcc_type::pick(const pick_context& ctx,
                                std::vector<feature>& out) const
        {
            if(!ctx.vis.altitude.any()) return;
            for(const auto& a : ctx.db.query_artcc(ctx.click_box))
            {
                if(!ctx.styles.artcc_visible(a.altitude, a.type, ctx.zoom)) continue;
                if(!altitude_filter_allows(ctx.vis.altitude, artcc_bands(a.altitude)))
                    continue;
                if(point_in_ring(ctx.click_lon, ctx.click_lat, a.points))
                    out.push_back(a);
            }
        }

        void tfr_type::pick(const pick_context& ctx,
                              std::vector<feature>& out) const
        {
            if(!ctx.styles.tfr_visible(ctx.zoom)) return;
            if(!ctx.vis.altitude.any()) return;
            for(const auto& t : ctx.db.query_tfr(ctx.click_box))
            {
                for(const auto& area : t.areas)
                {
                    if(!ctx.vis.altitude.overlaps(area.lower_ft_val, area.lower_ft_ref,
                                                   area.upper_ft_val, area.upper_ft_ref))
                        continue;
                    if(point_in_ring(ctx.click_lon, ctx.click_lat, area.points))
                    {
                        tfr pick = t;
                        pick.areas.clear();
                        pick.areas.push_back(area);
                        out.push_back(std::move(pick));
                    }
                }
            }
        }

        void adiz_type::pick(const pick_context& ctx,
                               std::vector<feature>& out) const
        {
            if(!ctx.styles.adiz_visible(ctx.zoom)) return;
            for(const auto& a : ctx.db.query_adiz(ctx.click_box))
            {
                for(const auto& part : a.parts)
                {
                    if(point_in_ring(ctx.click_lon, ctx.click_lat, part))
                    {
                        out.push_back(a);
                        break;
                    }
                }
            }
        }

        void pja_type::pick(const pick_context& ctx,
                              std::vector<feature>& out) const
        {
            if(!ctx.vis.altitude.any()) return;
            auto area_vis = ctx.styles.pja_area_visible(ctx.zoom);
            auto point_vis = ctx.styles.pja_point_visible(ctx.zoom);
            if(!area_vis && !point_vis) return;
            for(const auto& p : ctx.db.query_pjas(ctx.pick_box))
            {
                auto upper = p.max_altitude_ft_msl > 0 ? p.max_altitude_ft_msl
                                                        : altitude_filter::UNLIMITED_FT;
                if(!ctx.vis.altitude.overlaps(0, upper)) continue;
                auto is_point = (p.radius_nm <= 0);
                if(is_point ? !point_vis : !area_vis) continue;
                auto hit = is_point
                    ? (p.lon >= ctx.pick_box.lon_min && p.lon <= ctx.pick_box.lon_max
                    && p.lat >= ctx.pick_box.lat_min && p.lat <= ctx.pick_box.lat_max)
                    : point_in_circle_nm(ctx.click_lon, ctx.click_lat, p.lon, p.lat, p.radius_nm);
                if(hit) out.push_back(p);
            }
        }

        void maa_type::pick(const pick_context& ctx,
                              std::vector<feature>& out) const
        {
            if(!ctx.vis.altitude.low_enabled()) return;
            auto area_vis = ctx.styles.maa_area_visible(ctx.zoom);
            auto point_vis = ctx.styles.maa_point_visible(ctx.zoom);
            if(!area_vis && !point_vis) return;
            for(const auto& m : ctx.db.query_maas(ctx.click_box))
            {
                if(!m.shape.empty() && area_vis)
                {
                    if(point_in_ring(ctx.click_lon, ctx.click_lat, m.shape))
                        out.push_back(m);
                }
                else if(m.radius_nm > 0)
                {
                    auto is_area = m.shape.empty();
                    if(is_area ? area_vis : point_vis)
                        if(point_in_circle_nm(ctx.click_lon, ctx.click_lat, m.lon, m.lat, m.radius_nm))
                            out.push_back(m);
                }
            }
        }

        void airway_type::pick(const pick_context& ctx,
                                 std::vector<feature>& out) const
        {
            if(!ctx.vis.altitude.any()) return;
            for(const auto& seg : ctx.db.query_airways(ctx.pick_box))
            {
                if(!ctx.styles.airway_visible(seg.awy_id, ctx.zoom)) continue;
                if(!altitude_filter_allows(ctx.vis.altitude, airway_bands(seg.awy_id)))
                    continue;
                auto d = point_to_segment_nm(ctx.click_lon, ctx.click_lat,
                    seg.from_lon, seg.from_lat, seg.to_lon, seg.to_lat);
                if(d <= ctx.pick_radius_nm) out.push_back(seg);
            }
        }

        void mtr_type::pick(const pick_context& ctx,
                              std::vector<feature>& out) const
        {
            if(!ctx.vis.altitude.any() || !ctx.styles.mtr_visible(ctx.zoom)) return;
            for(const auto& seg : ctx.db.query_mtrs(ctx.pick_box))
            {
                if(!altitude_filter_allows(ctx.vis.altitude, mtr_bands(seg.route_type_code)))
                    continue;
                auto d = point_to_segment_nm(ctx.click_lon, ctx.click_lat,
                    seg.from_lon, seg.from_lat, seg.to_lon, seg.to_lat);
                if(d <= ctx.pick_radius_nm) out.push_back(seg);
            }
        }

        void runway_type::pick(const pick_context& ctx,
                                 std::vector<feature>& out) const
        {
            if(!ctx.vis.altitude.low_enabled() ||
               !ctx.styles.runway_visible(ctx.zoom)) return;
            for(const auto& rwy : ctx.db.query_runways(ctx.pick_box))
            {
                auto d = point_to_segment_nm(ctx.click_lon, ctx.click_lat,
                    rwy.end1_lon, rwy.end1_lat, rwy.end2_lon, rwy.end2_lat);
                if(d <= ctx.pick_radius_nm) out.push_back(rwy);
            }
        }

        // -- summary() bodies ------------------------------------------------

        std::string airport_type::summary(const feature& f) const
        {
            const auto& v = std::get<airport>(f);
            return "Airport: " + v.arpt_id + " " + v.arpt_name;
        }
        std::string navaid_type::summary(const feature& f) const
        {
            const auto& v = std::get<navaid>(f);
            return "Navaid: " + v.nav_id + " " + v.name;
        }
        std::string fix_type::summary(const feature& f) const
        {
            return "Fix: " + std::get<fix>(f).fix_id;
        }
        std::string obstacle_type::summary(const feature& f) const
        {
            const auto& v = std::get<obstacle>(f);
            return "Obstacle: " + v.oas_num + " " + fmt_int(v.agl_ht) + "ft AGL";
        }
        std::string airspace_type::summary(const feature& f) const
        {
            const auto& v = std::get<class_airspace>(f);
            return "Airspace: Class " + v.airspace_class + " " + v.name;
        }
        std::string sua_type::summary(const feature& f) const
        {
            const auto& v = std::get<sua>(f);
            return "SUA: " + v.sua_type + " " + v.name;
        }
        std::string artcc_type::summary(const feature& f) const
        {
            const auto& v = std::get<artcc>(f);
            return "ARTCC: " + v.location_id + " " + v.name;
        }
        std::string tfr_type::summary(const feature& f) const
        {
            const auto& v = std::get<tfr>(f);
            return "TFR: " + v.notam_id + " " + v.tfr_type;
        }
        std::string adiz_type::summary(const feature& f) const
        {
            return "ADIZ: " + std::get<adiz>(f).name;
        }
        std::string maa_type::summary(const feature& f) const
        {
            const auto& v = std::get<maa>(f);
            std::string s = "MAA: " + (!v.name.empty() ? v.name : v.maa_id);
            if(!v.type.empty()) s += " [" + v.type + "]";
            return s;
        }
        std::string pja_type::summary(const feature& f) const
        {
            const auto& v = std::get<pja>(f);
            return "PJA: " + (!v.name.empty() ? v.name : v.pja_id);
        }
        std::string awos_type::summary(const feature& f) const
        {
            const auto& v = std::get<awos>(f);
            return "AWOS: " + v.id + " " + v.type;
        }
        std::string comm_outlet_type::summary(const feature& f) const
        {
            const auto& v = std::get<comm_outlet>(f);
            return v.comm_type + ": " + v.outlet_name + " (" + v.facility_name + ")";
        }
        std::string airway_type::summary(const feature& f) const
        {
            const auto& v = std::get<airway_segment>(f);
            return "Airway: " + v.awy_id + " " + v.from_point + "-" + v.to_point;
        }
        std::string mtr_type::summary(const feature& f) const
        {
            const auto& v = std::get<mtr_segment>(f);
            return "MTR: " + v.mtr_id + " " + v.from_point + "-" + v.to_point;
        }
        std::string runway_type::summary(const feature& f) const
        {
            return "Runway: " + std::get<runway>(f).rwy_id;
        }

        // -- info_kv() bodies ------------------------------------------------

        kv_list airport_type::info_kv(const feature& f) const
        {
            const auto& v = std::get<airport>(f);
            return {
                {"ID",             v.arpt_id},
                {"Name",           v.arpt_name},
                {"City",           v.city},
                {"State",          v.state_code},
                {"Elevation",      fmt_dbl(v.elev, 0) + " ft"},
                {"TPA",            v.tpa},
                {"Airspace class", v.airspace_class},
                {"Tower type",     v.twr_type_code},
                {"ICAO ID",        v.icao_id},
                {"Ownership",      v.ownership_type_code},
                {"Facility use",   v.facility_use_code},
                {"Status",         v.arpt_status},
                {"Fuel types",     v.fuel_types},
                {"Mag var",        v.mag_varn.empty() ? std::string() : v.mag_varn + v.mag_hemis},
            };
        }

        kv_list navaid_type::info_kv(const feature& f) const
        {
            const auto& v = std::get<navaid>(f);
            return {
                {"ID",         v.nav_id},
                {"Name",       v.name},
                {"Type",       v.nav_type},
                {"City",       v.city},
                {"State",      v.state_code},
                {"Elevation",  fmt_dbl(v.elev, 0) + " ft"},
                {"Frequency",  v.freq},
                {"Channel",    v.chan},
                {"Power",      v.pwr_output},
                {"Hours",      v.oper_hours},
                {"Status",     v.nav_status},
                {"Low ARTCC",  v.low_alt_artcc_id},
                {"High ARTCC", v.high_alt_artcc_id},
            };
        }

        kv_list fix_type::info_kv(const feature& f) const
        {
            const auto& v = std::get<fix>(f);
            return {
                {"ID",          v.fix_id},
                {"State",       v.state_code},
                {"ICAO region", v.icao_region_code},
                {"Use code",    v.use_code},
                {"Low ARTCC",   v.artcc_id_low},
                {"High ARTCC",  v.artcc_id_high},
            };
        }

        kv_list obstacle_type::info_kv(const feature& f) const
        {
            const auto& v = std::get<obstacle>(f);
            return {
                {"OAS number",    v.oas_num},
                {"Type",          v.obstacle_type},
                {"Quantity",      fmt_int(v.quantity)},
                {"AGL height",    fmt_int(v.agl_ht) + " ft"},
                {"MSL height",    fmt_int(v.amsl_ht) + " ft"},
                {"Lighting",      v.lighting},
                {"Marking",       v.marking},
                {"City",          v.city},
                {"State",         v.state},
                {"Verify status", v.verify_status},
            };
        }

        kv_list airspace_type::info_kv(const feature& f) const
        {
            const auto& v = std::get<class_airspace>(f);
            return {
                {"Name",       v.name},
                {"Class",      v.airspace_class},
                {"Local type", v.local_type},
                {"Ident",      v.ident},
                {"Sector",     v.sector},
                {"Upper",      v.upper_ref.empty() ? std::string() : std::to_string(v.upper_ft) + " " + v.upper_ref},
                {"Lower",      v.lower_ref.empty() ? std::string() : std::to_string(v.lower_ft) + " " + v.lower_ref},
            };
        }

        kv_list sua_type::info_kv(const feature& f) const
        {
            const auto& v = std::get<sua>(f);
            kv_list rows{
                {"Designator", v.designator},
                {"Name",       v.name},
                {"Type",       v.sua_type},
                {"Upper",      v.upper_limit},
                {"Lower",      v.lower_limit},
            };
            if(!v.min_alt_limit.empty()) rows.push_back({"Min altitude", v.min_alt_limit});
            if(!v.max_alt_limit.empty()) rows.push_back({"Max altitude", v.max_alt_limit});
            if(v.conditional_exclusion == "YES")
                rows.push_back({"Conditional exclusion", "see legal note"});
            if(v.traffic_allowed == "MIL")
                rows.push_back({"Traffic allowed", "MIL only"});
            if(!v.time_in_advance_hr.empty())
                rows.push_back({"NOTAM lead time", v.time_in_advance_hr + " hr"});
            rows.push_back({"Controlling authority", v.controlling_authority});
            rows.push_back({"Admin area",            v.admin_area});
            rows.push_back({"Activity",              v.activity});
            rows.push_back({"Working hours",         v.working_hours});
            rows.push_back({"Status",                v.status});
            rows.push_back({"ICAO compliant",        v.icao_compliant});

            // Schedule endpoint: clock time, solar-event token, or event+offset.
            auto fmt_endpoint = [](const std::string& clock,
                                   const std::string& event,
                                   const std::string& offset_min) -> std::string {
                if(!clock.empty()) return clock;
                if(event.empty()) return {};
                if(offset_min.empty()) return event;
                std::string off = offset_min;
                auto dot = off.find('.');
                if(dot != std::string::npos) off.resize(dot);
                if(!off.empty() && off[0] != '-' && off[0] != '+') off = "+" + off;
                return event + off;
            };

            // Frequencies — one row per allocated channel.
            for(const auto& fr : v.freqs)
            {
                std::string val = (fr.tx == fr.rx || fr.rx.empty())
                                  ? fr.tx : (fr.tx + "/" + fr.rx);
                if(!fr.mode.empty() && fr.mode != "OTHER") val += " " + fr.mode;
                std::string flags;
                if(!fr.comm_allowed.empty()) flags = fr.comm_allowed;
                if(!fr.charted.empty() && fr.charted != "YES")
                {
                    if(!flags.empty()) flags += ", ";
                    flags += "uncharted";
                }
                if(!flags.empty()) val += " (" + flags + ")";
                if(!fr.sectors.empty()) val += " — sector " + fr.sectors;
                rows.push_back({"Frequency", val});
            }
            for(const auto& sc : v.schedules)
            {
                std::string day = sc.day;
                if(!sc.day_til.empty()) day += "-" + sc.day_til;
                auto start = fmt_endpoint(sc.start_time, sc.start_event, sc.start_event_offset);
                auto end = fmt_endpoint(sc.end_time, sc.end_event, sc.end_event_offset);
                std::string val = day;
                if(!start.empty() || !end.empty())
                {
                    val += " ";
                    val += start;
                    val += "-";
                    val += end;
                }
                rows.push_back({"Schedule", val});
            }
            rows.push_back({"Legal note", v.legal_note});
            return rows;
        }

        kv_list artcc_type::info_kv(const feature& f) const
        {
            const auto& v = std::get<artcc>(f);
            kv_list rows{
                {"Location ID",   v.location_id},
                {"Name",          v.name},
                {"ICAO ID",       v.icao_id},
                {"Location type", v.location_type},
                {"City",          v.city},
                {"State",         v.state},
                {"Country",       v.country_code},
                {"Altitude band", v.altitude},
                {"Type",          v.type},
            };
            if(!v.cross_ref.empty())
                rows.push_back({"Cross ref", v.cross_ref});
            return rows;
        }

        kv_list tfr_type::info_kv(const feature& f) const
        {
            const auto& v = std::get<tfr>(f);
            kv_list rows{
                {"NOTAM ID",  v.notam_id},
                {"Type",      v.tfr_type},
                {"Facility",  v.facility},
                {"Effective", v.date_effective},
                {"Expires",   v.date_expire},
            };
            for(const auto& area : v.areas)
            {
                if(!area.area_name.empty())
                    rows.push_back({"Area", area.area_name});
                std::string alt = fmt_int(area.lower_ft_val) + " " + area.lower_ft_ref
                                + " - " + fmt_int(area.upper_ft_val) + " " + area.upper_ft_ref;
                rows.push_back({"Altitude", alt});
                if(!area.date_effective.empty())
                    rows.push_back({"Area effective", area.date_effective});
                if(!area.date_expire.empty())
                    rows.push_back({"Area expires", area.date_expire});
            }
            rows.push_back({"Description", v.description});
            return rows;
        }
        kv_list adiz_type::info_kv(const feature& f) const
        {
            const auto& v = std::get<adiz>(f);
            return {
                {"Name",          v.name},
                {"Location",      v.location},
                {"Working hours", v.working_hours},
                {"Military",      v.military},
            };
        }

        kv_list maa_type::info_kv(const feature& f) const
        {
            const auto& v = std::get<maa>(f);
            return {
                {"ID",      v.maa_id},
                {"Name",    v.name},
                {"Type",    v.type},
                {"Min alt", std::to_string(v.min_alt_ft) + " " + v.min_alt_ref},
                {"Max alt", std::to_string(v.max_alt_ft) + " " + v.max_alt_ref},
                {"Radius",  fmt_dbl(v.radius_nm, 1) + " NM"},
            };
        }

        kv_list pja_type::info_kv(const feature& f) const
        {
            const auto& v = std::get<pja>(f);
            return {
                {"ID",           v.pja_id},
                {"Name",         v.name},
                {"Max altitude", v.max_altitude},
                {"Radius",       fmt_dbl(v.radius_nm, 1) + " NM"},
            };
        }

        kv_list awos_type::info_kv(const feature& f) const
        {
            const auto& v = std::get<awos>(f);
            return {
                {"ID",           v.id},
                {"Type",         v.type},
                {"City",         v.city},
                {"State",        v.state_code},
                {"Elevation",    fmt_dbl(v.elev, 0) + " ft"},
                {"Phone",        v.phone_no},
                {"Second phone", v.second_phone_no},
                {"Commissioned", v.commissioned_date},
                {"Remark",       v.remark},
            };
        }

        kv_list comm_outlet_type::info_kv(const feature& f) const
        {
            const auto& v = std::get<comm_outlet>(f);
            return {
                {"Outlet name",  v.outlet_name},
                {"Type",         v.comm_type},
                {"Facility ID",  v.facility_id},
                {"Facility name",v.facility_name},
                {"Hours",        v.opr_hrs},
                {"Status",       v.comm_status_code},
                {"City",         v.city},
                {"State",        v.state_code},
                {"Remark",       v.remark},
            };
        }

        kv_list airway_type::info_kv(const feature& f) const
        {
            const auto& v = std::get<airway_segment>(f);
            return {
                {"ID",           v.awy_id},
                {"Location",     v.awy_location},
                {"From",         v.from_point},
                {"To",           v.to_point},
                {"MEA",          v.min_enroute_alt},
                {"Mag crs/dist", v.mag_course_dist},
                {"Gap flag",     v.gap_flag},
            };
        }

        kv_list mtr_type::info_kv(const feature& f) const
        {
            const auto& v = std::get<mtr_segment>(f);
            return {
                {"ID",         v.mtr_id},
                {"Route type", v.route_type_code},
                {"From",       v.from_point},
                {"To",         v.to_point},
            };
        }

        kv_list runway_type::info_kv(const feature& f) const
        {
            const auto& v = std::get<runway>(f);
            return {
                {"ID",      v.rwy_id},
                {"Site no", v.site_no},
            };
        }

        // -- Icon emitters --------------------------------------------------

        // Label placement priorities (higher = placed first in overlap pass).
        constexpr auto LABEL_PRIORITY_AIRPORT_TOWERED   = 100;
        constexpr auto LABEL_PRIORITY_AIRPORT_UNTOWERED = 80;
        constexpr auto LABEL_PRIORITY_NAVAID_VOR        = 60;
        constexpr auto LABEL_PRIORITY_NAVAID_NDB        = 40;
        constexpr auto LABEL_PRIORITY_AIRWAY              = 50;
        constexpr auto LABEL_PRIORITY_FIX_AIRWAY        = 30;
        constexpr auto LABEL_PRIORITY_FIX_OTHER         = 20;
        constexpr auto LABEL_PRIORITY_AIRSPACE          = 10;

        constexpr auto AIRSPACE_LABEL_MIN_ZOOM = 10;

        uint8_t to_u8(float f) { return static_cast<uint8_t>(f * 255.0F); }

        geo_bbox request_bbox(const feature_build_request& req)
        {
            return {req.lon_min, req.lat_min, req.lon_max, req.lat_max};
        }

        void emit_airspace_label(
            const build_context& ctx, const std::string& type_text,
            double mx, double my, int layer,
            const std::string& upper, const std::string& lower,
            const feature_style& fs)
        {
            ctx.labels.push_back({
                .text = type_text,
                .mx = mx, .my = my,
                .priority = LABEL_PRIORITY_AIRSPACE,
                .layer = layer,
                .upper_text = upper,
                .lower_text = lower,
                .outline_r = to_u8(fs.r),
                .outline_g = to_u8(fs.g),
                .outline_b = to_u8(fs.b),
            });
        }

        bool is_military_apt(const airport& apt)
        {
            return apt.ownership_type_code == "MA" ||
                   apt.ownership_type_code == "MR" ||
                   apt.ownership_type_code == "MN" ||
                   apt.ownership_type_code == "CG";
        }

        void emit_airport_icon(polyline_data& pd, double cx, double cy,
                                double r, double pixels_per_world,
                                const airport& apt, const feature_style& cs)
        {
            constexpr auto APT_OUTER_SCALE = 1.2;
            constexpr auto APT_RING_WIDTH_PX = 1.0F;
            constexpr auto APT_FILL_RADIUS = 0.5;

            auto symbol_r = r * APT_OUTER_SCALE;
            auto ring_geom_r = symbol_r - (APT_RING_WIDTH_PX * 0.5) / pixels_per_world;
            auto ring_ls = line_style{APT_RING_WIDTH_PX, 1.0F, 0, 0, cs.r, cs.g, cs.b, cs.a, 0};

            auto closed = apt.arpt_status == "CI" || apt.arpt_status == "CP";
            auto pvt = apt.facility_use_code == "PR";
            auto mil = is_military_apt(apt);
            auto h = ring_geom_r * LETTER_HEIGHT;
            auto white_ls = line_style{LETTER_WIDTH_PX, 0, 0, 0, 1.0F, 1.0F, 1.0F, 1.0F, 0};
            auto w = h * LETTER_ASPECT;

            const std::vector<float>* letter = nullptr;
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
                auto geom_r = symbol_r * APT_FILL_RADIUS;
                auto fill_px = static_cast<float>(symbol_r * pixels_per_world);
                auto fill_ls = line_style{fill_px, 1.0F, 0, 0, cs.r, cs.g, cs.b, cs.a, 0};
                add_circle_to(pd, cx, cy, geom_r, fill_ls);
                if(letter) add_letter(pd, *letter, cx, cy, w, h, white_ls);
            }
            else if(letter)
            {
                auto filled_ring = ring_ls;
                filled_ring.fill_width = SYMBOL_FILL_PX;
                add_circle_to(pd, cx, cy, ring_geom_r, filled_ring);
                add_letter(pd, *letter, cx, cy, w, h, white_ls);
            }
            else
            {
                add_circle_to(pd, cx, cy, ring_geom_r, ring_ls);
            }
        }

        void emit_navaid_icon(polyline_data& pd, double cx, double cy, double r,
                               const navaid& nav, const feature_style& fs)
        {
            constexpr auto NAV_NDB_CIRCLE = 0.4;
            constexpr auto NAV_DME_RECT = 0.85;
            constexpr auto NAV_VORDME_WIDTH = 1.1;

            auto ls = to_line_style(fs);
            auto filled_ls = ls;
            filled_ls.fill_width = SYMBOL_FILL_PX;

            if(nav.nav_type == "NDB")
                add_circle(pd, cx, cy, r * NAV_NDB_CIRCLE, filled_ls);
            else if(nav.nav_type == "NDB/DME")
            {
                add_circle(pd, cx, cy, r * NAV_NDB_CIRCLE, filled_ls);
                add_rect(pd, cx, cy, r * NAV_DME_RECT, r * NAV_DME_RECT, ls);
            }
            else if(nav.nav_type == "DME")
                add_rect(pd, cx, cy, r * NAV_DME_RECT, r * NAV_DME_RECT, filled_ls);
            else if(nav.nav_type == "VOR/DME")
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

        // --- Fix-specific icon shapes ---

        void add_triangle_polyline(polyline_data& pd, double cx, double cy,
                                    double r, const line_style& ls)
        {
            constexpr auto SQRT3_2 = 0.866;
            auto h = r * SQRT3_2;
            auto fcx = static_cast<float>(cx);
            auto top_y = static_cast<float>(cy + r);
            auto bot_y = static_cast<float>(cy - r * 0.5);
            auto left_x = static_cast<float>(cx - h);
            auto right_x = static_cast<float>(cx + h);
            pd.polylines.push_back({
                {fcx, top_y}, {left_x, bot_y},
                {right_x, bot_y}, {fcx, top_y},
            });
            pd.styles.push_back(ls);
        }

        void add_waypoint_star_polyline(polyline_data& pd,
                                         double cx, double cy, double r,
                                         const line_style& ls)
        {
            constexpr auto ARC_SEGS = 3;
            constexpr auto SQRT2_M1 = 0.41421356;

            struct arc_def { double acx, acy; double start_angle; };
            const std::array<arc_def, 4> arcs = {{
                { r,  r, -M_PI / 2}, {-r,  r,  0},
                {-r, -r,  M_PI / 2}, { r, -r,  M_PI},
            }};

            std::vector<glm::vec2> star_pts;
            for(const auto& [acx_off, acy_off, a0] : arcs)
            {
                for(int i = 0; i <= ARC_SEGS; i++)
                {
                    auto t = static_cast<double>(i) / ARC_SEGS;
                    auto angle = a0 - t * (M_PI / 2);
                    auto px = cx + acx_off + r * std::cos(angle);
                    auto py = cy + acy_off + r * std::sin(angle);
                    star_pts.push_back({static_cast<float>(px),
                                        static_cast<float>(py)});
                }
            }
            star_pts.push_back(star_pts[0]);
            pd.polylines.push_back(std::move(star_pts));
            pd.styles.push_back(ls);

            constexpr auto CIRCLE_SEGS = 8;
            auto cr = r * SQRT2_M1;
            std::vector<glm::vec2> circle_pts;
            for(int i = 0; i <= CIRCLE_SEGS; i++)
            {
                auto angle = 2 * M_PI * static_cast<double>(i) / CIRCLE_SEGS;
                circle_pts.push_back({static_cast<float>(cx + cr * std::cos(angle)),
                                      static_cast<float>(cy + cr * std::sin(angle))});
            }
            pd.polylines.push_back(std::move(circle_pts));
            pd.styles.push_back(ls);
        }

        void emit_fix_icon(polyline_data& pd, double cx, double cy, double r,
                           const fix& f, const feature_style& fs)
        {
            auto ls = to_line_style(fs);
            ls.fill_width = SYMBOL_FILL_PX;
            if(f.use_code == "RP" || f.use_code == "MR")
                add_triangle_polyline(pd, cx, cy, r, ls);
            else
                add_waypoint_star_polyline(pd, cx, cy, r, ls);
        }

        // --- Obstacle icon ---

        void add_obstacle_polylines(polyline_data& pd, double cx, double cy,
                                     double radius, bool tall, bool lighted,
                                     const line_style& ls)
        {
            auto dot_r = radius * 0.2;
            auto half_w = radius * 0.7;
            auto leg_y = cy - dot_r;
            auto apex_y = leg_y + radius * 1.6;
            auto mast_top = tall ? apex_y + radius * 0.8 : apex_y;

            auto fcx = static_cast<float>(cx);
            auto fleg_y = static_cast<float>(leg_y);
            auto fapex_y = static_cast<float>(apex_y);
            auto fmast_top = static_cast<float>(mast_top);
            auto fleft = static_cast<float>(cx - half_w);
            auto fright = static_cast<float>(cx + half_w);

            if(tall)
            {
                pd.polylines.push_back({
                    glm::vec2(fleft, fleg_y),
                    glm::vec2(fcx, fapex_y), glm::vec2(fcx, fmast_top),
                    glm::vec2(fcx, fapex_y), glm::vec2(fright, fleg_y),
                });
            }
            else
            {
                pd.polylines.push_back({
                    glm::vec2(fleft, fleg_y), glm::vec2(fcx, fapex_y),
                    glm::vec2(fright, fleg_y),
                });
            }
            pd.styles.push_back(ls);

            add_circle_to(pd, cx, cy, dot_r, ls, 8);

            if(lighted)
            {
                auto gap = radius * 0.4;
                auto ray_len = radius * 0.15;
                constexpr auto DEG_TO_RAD = M_PI / 180.0;
                constexpr std::array<double, 5> angles = {-120, -60, 0, 60, 120};
                for(auto deg : angles)
                {
                    auto rad = (90.0 - deg) * DEG_TO_RAD;
                    auto dx = std::cos(rad);
                    auto dy = std::sin(rad);
                    auto x0 = cx + dx * gap;
                    auto y0 = mast_top + dy * gap;
                    auto x1 = cx + dx * (gap + ray_len);
                    auto y1 = mast_top + dy * (gap + ray_len);
                    add_seg_to(pd, x0, y0, x1, y1, ls);
                }
            }
        }

        void emit_obstacle_icon(polyline_data& pd, double cx, double cy, double r,
                                 const obstacle& obs, const feature_style& fs)
        {
            auto ls = to_line_style(fs);
            auto lighted = obs.lighting != "N";
            add_obstacle_polylines(pd, cx, cy, r, obs.agl_ht >= 1000, lighted, ls);
        }

        // --- PJA / MAA point icons (diamond with a letter) ---

        void emit_pja_point_icon(polyline_data& pd, double cx, double cy, double r,
                                  const feature_style& fs)
        {
            auto ls = to_line_style(fs);
            ls.fill_width = SYMBOL_FILL_PX;
            auto fcx = static_cast<float>(cx);
            auto fcy = static_cast<float>(cy);
            auto fright = static_cast<float>(cx + r);
            auto fleft = static_cast<float>(cx - r);
            auto fup = static_cast<float>(cy + r);
            auto fdown = static_cast<float>(cy - r);
            pd.polylines.push_back({
                {fright, fcy}, {fcx, fup}, {fleft, fcy},
                {fcx, fdown}, {fright, fcy},
            });
            pd.styles.push_back(ls);

            auto lh = r * LETTER_HEIGHT;
            auto lw = lh * LETTER_ASPECT;
            auto white_ls = line_style{LETTER_WIDTH_PX, 0, 0, 0, 1, 1, 1, 1, 0};
            add_letter(pd, letter_P, cx, cy, lw, lh, white_ls);
        }

        void emit_maa_point_icon(polyline_data& pd, double cx, double cy, double r,
                                  const maa& m, const feature_style& fs)
        {
            auto ls = to_line_style(fs);
            ls.fill_width = SYMBOL_FILL_PX;
            auto fcx = static_cast<float>(cx);
            auto fcy = static_cast<float>(cy);
            auto fright = static_cast<float>(cx + r);
            auto fleft = static_cast<float>(cx - r);
            auto fup = static_cast<float>(cy + r);
            auto fdown = static_cast<float>(cy - r);
            pd.polylines.push_back({
                {fright, fcy}, {fcx, fup}, {fleft, fcy},
                {fcx, fdown}, {fright, fcy},
            });
            pd.styles.push_back(ls);

            // Every MAA type in the current NASR data has a glyph. If
            // a future release adds a new category, fail loudly so we
            // pick a glyph instead of silently rendering a naked diamond.
            const std::vector<float>& ld = [&]() -> const std::vector<float>& {
                if(m.type == "AEROBATIC PRACTICE") return letter_A;
                if(m.type == "GLIDER")             return letter_G;
                if(m.type == "HANG GLIDER")        return letter_H;
                if(m.type == "ULTRALIGHT")         return letter_U;
                if(m.type == "SPACE LAUNCH")       return letter_S;
                throw std::runtime_error("Unknown MAA type: " + m.type);
            }();

            auto lh = r * LETTER_HEIGHT;
            auto lw = lh * LETTER_ASPECT;
            auto white_ls = line_style{LETTER_WIDTH_PX, 0, 0, 0, 1, 1, 1, 1, 0};
            add_letter(pd, ld, cx, cy, lw, lh, white_ls);
        }

        void emit_comm_icon(polyline_data& pd, double cx, double cy, double r,
                             const line_style& ls)
        {
            add_comm_symbol(pd, cx, cy, r * 0.75, ls);
        }

        // -- build() bodies -------------------------------------------------

        void airport_type::build(const build_context& ctx) const
        {
            if(!ctx.styles.any_airport_visible(ctx.req.zoom)) return;
            const auto& airports = ctx.db.query_airports(
                request_bbox(ctx.req),
                ctx.styles.visible_airport_classes(ctx.req.zoom));

            auto r = ctx.req.half_extent_y * SYMBOL_RADIUS_AIRPORT;
            auto pixels_per_world =
                ctx.req.viewport_height / (2.0 * ctx.req.half_extent_y);

            for(const auto& apt : airports)
            {
                if(!ctx.styles.airport_visible(apt, ctx.req.zoom)) continue;

                const auto& cs = ctx.styles.airport_style(apt);
                auto cx = lon_to_mx(apt.lon) + ctx.mx_offset;
                auto cy = lat_to_my(apt.lat);

                emit_airport_icon(ctx.poly[layer_airports], cx, cy, r,
                                   pixels_per_world, apt, cs);

                const auto& id = apt.icao_id.empty() ? apt.arpt_id : apt.icao_id;
                auto towered = apt.twr_type_code.find("ATCT") != std::string::npos;
                ctx.labels.push_back({
                    .text = id,
                    .mx = lon_to_mx(apt.lon) + ctx.mx_offset,
                    .my = lat_to_my(apt.lat),
                    .priority = towered ? LABEL_PRIORITY_AIRPORT_TOWERED
                                        : LABEL_PRIORITY_AIRPORT_UNTOWERED,
                    .layer = layer_airports,
                });
            }
        }

        void runway_type::build(const build_context& ctx) const
        {
            if(!ctx.styles.runway_visible(ctx.req.zoom)) return;
            if(!ctx.req.altitude.low_enabled()) return;

            const auto& runways = ctx.db.query_runways(
                request_bbox(ctx.req));
            const auto& fs = ctx.styles.runway_style();

            for(const auto& rwy : runways)
            {
                auto x0 = lon_to_mx(rwy.end1_lon) + ctx.mx_offset;
                auto y0 = lat_to_my(rwy.end1_lat);
                auto x1 = lon_to_mx(rwy.end2_lon) + ctx.mx_offset;
                auto y1 = lat_to_my(rwy.end2_lat);
                ctx.poly[layer_runways].polylines.push_back({
                    glm::vec2(static_cast<float>(x0), static_cast<float>(y0)),
                    glm::vec2(static_cast<float>(x1), static_cast<float>(y1))});
                ctx.poly[layer_runways].styles.push_back(to_line_style(fs));
            }
        }

        void navaid_type::build(const build_context& ctx) const
        {
            if(!ctx.styles.any_navaid_visible(ctx.req.zoom)) return;
            if(!ctx.req.altitude.any()) return;
            constexpr auto NAV_CLEARANCE = 2.0;

            const auto& navaids = ctx.db.query_navaids(
                request_bbox(ctx.req));
            auto r = ctx.req.half_extent_y * SYMBOL_RADIUS_AIRPORT;

            ctx.state.navaid_positions.clear();
            ctx.state.navaid_clearance = static_cast<float>(r * NAV_CLEARANCE);

            for(const auto& nav : navaids)
            {
                if(nav.nav_type == "VOT" || nav.nav_type == "FAN MARKER" ||
                   nav.nav_type == "MARINE NDB") continue;
                if(!ctx.styles.navaid_visible(nav.nav_type, ctx.req.zoom)) continue;

                auto keep = (nav.is_low && ctx.req.altitude.show_low)
                         || (nav.is_high && ctx.req.altitude.show_high);
                if(!keep) continue;

                const auto& fs = ctx.styles.navaid_style(nav.nav_type);
                auto cx = lon_to_mx(nav.lon) + ctx.mx_offset;
                auto cy = lat_to_my(nav.lat);

                ctx.state.navaid_positions.emplace_back(
                    static_cast<float>(cx), static_cast<float>(cy));
                emit_navaid_icon(ctx.poly[layer_navaids], cx, cy, r, nav, fs);

                auto is_vor = nav.nav_type.find("VOR") != std::string::npos
                           || nav.nav_type == "TACAN" || nav.nav_type == "VORTAC";
                ctx.labels.push_back({
                    .text = nav.nav_id,
                    .mx = lon_to_mx(nav.lon) + ctx.mx_offset,
                    .my = lat_to_my(nav.lat),
                    .priority = is_vor ? LABEL_PRIORITY_NAVAID_VOR
                                       : LABEL_PRIORITY_NAVAID_NDB,
                    .layer = layer_navaids,
                });
            }
        }

        // Heading angle from Mercator dx/dy, normalized to [-pi/2, pi/2]
        // so text reads left-to-right with up-vector toward screen-up.
        float segment_angle(float dx, float dy)
        {
            auto a = std::atan2(dy, dx);
            if(a > static_cast<float>(M_PI_2))
                a -= static_cast<float>(M_PI);
            else if(a < static_cast<float>(-M_PI_2))
                a += static_cast<float>(M_PI);
            return a;
        }

        constexpr auto AIRWAY_LABEL_ANGLE_THRESHOLD = 30.0F * static_cast<float>(M_PI) / 180.0F;
        constexpr auto AIRWAY_LABEL_INTERVAL = 6;

        void airway_type::build(const build_context& ctx) const
        {
            ctx.state.airway_waypoints.clear();
            if(!ctx.styles.any_airway_visible(ctx.req.zoom)) return;
            const auto& airways = ctx.db.query_airways(
                request_bbox(ctx.req));

            // Visible segment info for label placement
            struct seg_info
            {
                int point_seq;
                double mid_mx, mid_my;
                float dx, dy;
            };
            std::unordered_map<std::string, std::vector<seg_info>> label_groups;

            for(const auto& seg : airways)
            {
                if(!ctx.styles.airway_visible(seg.awy_id, ctx.req.zoom)) continue;
                if(!altitude_filter_allows(ctx.req.altitude, airway_bands(seg.awy_id)))
                    continue;

                ctx.state.airway_waypoints.insert(seg.from_point);
                ctx.state.airway_waypoints.insert(seg.to_point);

                const auto& fs = ctx.styles.airway_style(seg.awy_id);

                auto from_mx = lon_to_mx(seg.from_lon) + ctx.mx_offset;
                auto from_my = lat_to_my(seg.from_lat);
                auto to_mx = lon_to_mx(seg.to_lon) + ctx.mx_offset;
                auto to_my = lat_to_my(seg.to_lat);

                auto arc = geodesic_interpolate(
                    seg.from_lat, seg.from_lon, seg.to_lat, seg.to_lon);
                std::vector<glm::vec2> polyline;
                polyline.reserve(arc.size());
                for(const auto& p : arc)
                    polyline.emplace_back(
                        static_cast<float>(lon_to_mx(p.lon) + ctx.mx_offset),
                        static_cast<float>(lat_to_my(p.lat)));

                // Trim endpoints away from navaid icons. Compute the unit
                // vector in double using the original Mercator endpoints so
                // the subtraction doesn't suffer from the polyline's float
                // quantization (matters for short segments where dx/dy are
                // small relative to absolute Mercator magnitude).
                auto& front = polyline.front();
                auto& back = polyline.back();
                auto dx = to_mx - from_mx;
                auto dy = to_my - from_my;
                auto len = std::sqrt(dx * dx + dy * dy);
                if(len > 0.0)
                {
                    auto ux = dx / len;
                    auto uy = dy / len;
                    auto clr = ctx.state.navaid_clearance;
                    if(ctx.is_at_navaid(front.x, front.y))
                    {
                        front.x = static_cast<float>(from_mx + ux * clr);
                        front.y = static_cast<float>(from_my + uy * clr);
                    }
                    if(ctx.is_at_navaid(back.x, back.y))
                    {
                        back.x = static_cast<float>(to_mx - ux * clr);
                        back.y = static_cast<float>(to_my - uy * clr);
                    }
                }
                if((back.x - front.x) * dx + (back.y - front.y) * dy <= 0.0)
                    continue;

                label_groups[seg.awy_id].push_back({
                    seg.point_seq,
                    (from_mx + to_mx) * 0.5,
                    (from_my + to_my) * 0.5,
                    static_cast<float>(to_mx - from_mx),
                    static_cast<float>(to_my - from_my),
                });

                ctx.poly[layer_airways].polylines.push_back(std::move(polyline));
                ctx.poly[layer_airways].styles.push_back(to_line_style(fs));
            }

            // Emit labels at regular intervals along each airway,
            // plus extras at significant direction changes.
            for(auto& [awy_id, segs] : label_groups)
            {
                std::sort(segs.begin(), segs.end(),
                    [](const seg_info& a, const seg_info& b)
                    { return a.point_seq < b.point_seq; });

                // Place labels every AIRWAY_LABEL_INTERVAL segments,
                // centered within each group.
                auto n = segs.size();
                auto start = (n % AIRWAY_LABEL_INTERVAL) / 2;
                std::unordered_set<size_t> labeled;
                for(size_t i = start; i < n; i += AIRWAY_LABEL_INTERVAL)
                {
                    labeled.insert(i);
                    const auto& s = segs[i];
                    ctx.labels.push_back({
                        .text = awy_id,
                        .mx = s.mid_mx, .my = s.mid_my,
                        .priority = LABEL_PRIORITY_AIRWAY,
                        .layer = layer_airways,
                        .angle = segment_angle(s.dx, s.dy),
                    });
                }
                // Always emit at least one label
                if(labeled.empty())
                {
                    const auto& s = segs[n / 2];
                    labeled.insert(n / 2);
                    ctx.labels.push_back({
                        .text = awy_id,
                        .mx = s.mid_mx, .my = s.mid_my,
                        .priority = LABEL_PRIORITY_AIRWAY,
                        .layer = layer_airways,
                        .angle = segment_angle(s.dx, s.dy),
                    });
                }

                // Additional labels at significant direction changes
                for(size_t i = 1; i < n; i++)
                {
                    if(labeled.count(i)) continue;
                    auto a0 = segment_angle(segs[i - 1].dx, segs[i - 1].dy);
                    auto a1 = segment_angle(segs[i].dx, segs[i].dy);
                    if(std::abs(a1 - a0) > AIRWAY_LABEL_ANGLE_THRESHOLD)
                    {
                        const auto& s = segs[i];
                        ctx.labels.push_back({
                            .text = awy_id,
                            .mx = s.mid_mx, .my = s.mid_my,
                            .priority = LABEL_PRIORITY_AIRWAY,
                            .layer = layer_airways,
                            .angle = segment_angle(s.dx, s.dy),
                        });
                    }
                }
            }
        }

        void fix_type::build(const build_context& ctx) const
        {
            if(!ctx.styles.any_fix_visible(ctx.req.zoom)) return;
            if(!ctx.req.altitude.any()) return;
            const auto& fixes = ctx.db.query_fixes(
                request_bbox(ctx.req));
            auto radius = ctx.req.half_extent_y * SYMBOL_RADIUS_FIX;

            for(const auto& f : fixes)
            {
                if(!ctx.styles.fix_visible(ctx.fix_on_airway(f.fix_id), ctx.req.zoom))
                    continue;
                auto keep = (f.is_low && ctx.req.altitude.show_low)
                         || (f.is_high && ctx.req.altitude.show_high);
                if(!keep) continue;

                const auto& fs = ctx.styles.fix_style(f.use_code);
                auto cx = lon_to_mx(f.lon) + ctx.mx_offset;
                auto cy = lat_to_my(f.lat);

                emit_fix_icon(ctx.poly[layer_fixes], cx, cy, radius, f, fs);

                auto on_airway = ctx.fix_on_airway(f.fix_id);
                ctx.labels.push_back({
                    .text = f.fix_id,
                    .mx = lon_to_mx(f.lon) + ctx.mx_offset,
                    .my = lat_to_my(f.lat),
                    .priority = on_airway ? LABEL_PRIORITY_FIX_AIRWAY
                                          : LABEL_PRIORITY_FIX_OTHER,
                    .layer = layer_fixes,
                });
            }
        }

        void mtr_type::build(const build_context& ctx) const
        {
            if(!ctx.styles.mtr_visible(ctx.req.zoom)) return;
            if(!ctx.req.altitude.any()) return;

            const auto& mtrs = ctx.db.query_mtrs(
                request_bbox(ctx.req));
            const auto& fs = ctx.styles.mtr_style();

            struct seg_info
            {
                double mid_mx, mid_my;
                float dx, dy;
            };
            std::unordered_map<std::string, std::vector<seg_info>> label_groups;

            for(const auto& seg : mtrs)
            {
                if(!altitude_filter_allows(ctx.req.altitude, mtr_bands(seg.route_type_code)))
                    continue;

                auto from_mx = lon_to_mx(seg.from_lon) + ctx.mx_offset;
                auto from_my = lat_to_my(seg.from_lat);
                auto to_mx = lon_to_mx(seg.to_lon) + ctx.mx_offset;
                auto to_my = lat_to_my(seg.to_lat);

                auto arc = geodesic_interpolate(
                    seg.from_lat, seg.from_lon, seg.to_lat, seg.to_lon);
                std::vector<glm::vec2> polyline;
                polyline.reserve(arc.size());
                for(const auto& p : arc)
                    polyline.emplace_back(
                        static_cast<float>(lon_to_mx(p.lon) + ctx.mx_offset),
                        static_cast<float>(lat_to_my(p.lat)));

                label_groups[seg.mtr_id].push_back({
                    (from_mx + to_mx) * 0.5,
                    (from_my + to_my) * 0.5,
                    static_cast<float>(to_mx - from_mx),
                    static_cast<float>(to_my - from_my),
                });

                ctx.poly[layer_mtrs].polylines.push_back(std::move(polyline));
                ctx.poly[layer_mtrs].styles.push_back(to_line_style(fs));
            }

            for(const auto& [mtr_id, segs] : label_groups)
            {
                auto median = segs.size() / 2;
                const auto& ms = segs[median];
                ctx.labels.push_back({
                    .text = mtr_id,
                    .mx = ms.mid_mx, .my = ms.mid_my,
                    .priority = LABEL_PRIORITY_AIRWAY,
                    .layer = layer_mtrs,
                    .angle = segment_angle(ms.dx, ms.dy),
                });
            }
        }

        // Format altitude value for airspace labels: round to nearest
        // 100 feet, zero-pad to 3 digits. AGL gets "A" suffix.
        std::string format_altitude(int ft_val, const std::string& ft_ref)
        {
            if(ft_val <= 0 && (ft_ref == "SFC" || ft_ref.empty()))
                return "SFC";
            if(ft_val < 0) return "";
            auto hundreds = (ft_val + 50) / 100;
            std::array<char, 16> buf{};
            if(ft_ref == "MSL" || ft_ref == "STD")
                std::snprintf(buf.data(), buf.size(), "%03d", hundreds);
            else
                std::snprintf(buf.data(), buf.size(), "%03dA", hundreds);
            return buf.data();
        }

        const char* sua_type_label(const std::string& sua_type)
        {
            if(sua_type == "PA") return "PRO";
            if(sua_type == "RA") return "RES";
            if(sua_type == "AA") return "ALR";
            if(sua_type == "WA") return "WRN";
            if(sua_type == "MOA") return "MOA";
            if(sua_type == "NSA") return "NSA";
            return sua_type.c_str();
        }

        // Pick a label position inside an airspace polygon: midpoint of
        // the longest edge, nudged 30% toward the centroid so the label
        // sits inside the boundary rather than on it.
        std::pair<double, double> polygon_label_pos(
            const std::vector<airspace_point>& pts, double mx_offset)
        {
            if(pts.size() < 2)
            {
                auto mx = lon_to_mx(pts[0].lon) + mx_offset;
                auto my = lat_to_my(pts[0].lat);
                return {mx, my};
            }

            // Centroid
            auto sum_mx = 0.0;
            auto sum_my = 0.0;
            for(const auto& p : pts)
            {
                sum_mx += lon_to_mx(p.lon) + mx_offset;
                sum_my += lat_to_my(p.lat);
            }
            auto n = static_cast<double>(pts.size());
            auto cx = sum_mx / n;
            auto cy = sum_my / n;

            // Longest edge midpoint
            auto best_len2 = 0.0;
            auto best_mx = cx;
            auto best_my = cy;
            for(size_t i = 0; i < pts.size(); i++)
            {
                auto j = (i + 1) % pts.size();
                auto ax = lon_to_mx(pts[i].lon) + mx_offset;
                auto ay = lat_to_my(pts[i].lat);
                auto bx = lon_to_mx(pts[j].lon) + mx_offset;
                auto by = lat_to_my(pts[j].lat);
                auto dx = bx - ax;
                auto dy = by - ay;
                auto len2 = dx * dx + dy * dy;
                if(len2 > best_len2)
                {
                    best_len2 = len2;
                    best_mx = (ax + bx) * 0.5;
                    best_my = (ay + by) * 0.5;
                }
            }

            // Blend 70% edge midpoint, 30% centroid
            constexpr auto EDGE_WEIGHT = 0.7;
            return {best_mx * EDGE_WEIGHT + cx * (1.0 - EDGE_WEIGHT),
                    best_my * EDGE_WEIGHT + cy * (1.0 - EDGE_WEIGHT)};
        }

        // Label position for a circular airspace: a point on the
        // circumference (upper-right quadrant), nudged 30% toward center.
        std::pair<double, double> circle_label_pos(
            double lat, double lon, double radius_nm, double mx_offset)
        {
            constexpr auto NM_TO_DEG_LAT = 1.0 / 60.0;
            auto edge_lat = lat + radius_nm * NM_TO_DEG_LAT * 0.707;
            auto edge_lon = lon + radius_nm * NM_TO_DEG_LAT * 0.707
                / std::cos(lat * M_PI / 180.0);
            auto cmx = lon_to_mx(lon) + mx_offset;
            auto cmy = lat_to_my(lat);
            auto emx = lon_to_mx(edge_lon) + mx_offset;
            auto emy = lat_to_my(edge_lat);
            constexpr auto EDGE_WEIGHT = 0.7;
            return {emx * EDGE_WEIGHT + cmx * (1.0 - EDGE_WEIGHT),
                    emy * EDGE_WEIGHT + cmy * (1.0 - EDGE_WEIGHT)};
        }

        void sua_type::build(const build_context& ctx) const
        {
            if(!ctx.styles.any_sua_visible(ctx.req.zoom)) return;
            if(!ctx.req.altitude.any()) return;
            auto sua_filter = ctx.styles.visible_sua_types(ctx.req.zoom);
            auto bbox = request_bbox(ctx.req);

            const auto& segs = ctx.db.query_sua_segments(bbox, sua_filter);
            for(const auto& seg : segs)
            {
                if(!ctx.styles.sua_visible(seg.sua_type, ctx.req.zoom)) continue;
                if(!sua_altitude_visible(ctx.req.altitude,
                                          seg.upper_ft_val, seg.upper_ft_ref,
                                          seg.lower_ft_val, seg.lower_ft_ref))
                    continue;
                auto ls = to_line_style(ctx.styles.sua_style(seg.sua_type));
                add_polyline(ctx.poly[layer_sua], seg.points, ls, ctx.mx_offset);
            }

            const auto& circles = ctx.db.query_sua_circles(bbox, sua_filter);
            for(const auto& c : circles)
            {
                if(!sua_altitude_visible(ctx.req.altitude,
                                          c.upper_ft_val, c.upper_ft_ref,
                                          c.lower_ft_val, c.lower_ft_ref))
                    continue;
                auto ls = to_line_style(ctx.styles.sua_style(c.sua_type));
                add_geodesic_circle(ctx.poly[layer_sua],
                    c.center_lat, c.center_lon, c.radius_nm,
                    ls, ctx.mx_offset);
            }

            // Labels from full SUA query
            if(ctx.req.zoom < AIRSPACE_LABEL_MIN_ZOOM) return;
            const auto& suas = ctx.db.query_sua(bbox, sua_filter);
            for(const auto& s : suas)
            {
                if(!ctx.styles.sua_visible(s.sua_type, ctx.req.zoom)) continue;
                if(s.strata.empty()) continue;
                const auto& st = s.strata[0];
                if(!sua_altitude_visible(ctx.req.altitude,
                                          st.upper_ft_val, st.upper_ft_ref,
                                          st.lower_ft_val, st.lower_ft_ref))
                    continue;
                if(st.parts.empty() || st.parts[0].points.empty()) continue;

                auto [cmx, cmy] = polygon_label_pos(st.parts[0].points,
                                                    ctx.mx_offset);
                emit_airspace_label(ctx, sua_type_label(s.sua_type),
                    cmx, cmy, layer_sua,
                    format_altitude(st.upper_ft_val, st.upper_ft_ref),
                    format_altitude(st.lower_ft_val, st.lower_ft_ref),
                    ctx.styles.sua_style(s.sua_type));
            }
        }

        void pja_type::build(const build_context& ctx) const
        {
            auto area_vis = ctx.styles.pja_area_visible(ctx.req.zoom);
            auto point_vis = ctx.styles.pja_point_visible(ctx.req.zoom);
            if(!area_vis && !point_vis) return;
            if(!ctx.req.altitude.any()) return;

            constexpr auto SYMBOL_RADIUS_PJA = SYMBOL_RADIUS_AIRPORT;

            const auto& pjas = ctx.db.query_pjas(
                request_bbox(ctx.req));

            for(const auto& p : pjas)
            {
                auto upper = p.max_altitude_ft_msl > 0 ? p.max_altitude_ft_msl
                                                        : altitude_filter::UNLIMITED_FT;
                if(!ctx.req.altitude.overlaps(0, upper)) continue;

                if(p.radius_nm > 0.0 && area_vis)
                {
                    auto ls = to_line_style(ctx.styles.pja_area_style());
                    add_geodesic_circle(ctx.poly[layer_pja],
                        p.lat, p.lon, p.radius_nm,
                        ls, ctx.mx_offset);

                    if(ctx.req.zoom >= AIRSPACE_LABEL_MIN_ZOOM)
                    {
                        auto [mx, my] = circle_label_pos(
                            p.lat, p.lon, p.radius_nm, ctx.mx_offset);
                        std::string upper = p.max_altitude_ft_msl > 0
                            ? format_altitude(p.max_altitude_ft_msl, "MSL")
                            : "UNL";
                        emit_airspace_label(ctx, "PJA", mx, my, layer_pja,
                            upper, "SFC", ctx.styles.pja_area_style());
                    }
                }
                else if(p.radius_nm <= 0.0 && point_vis)
                {
                    auto cx = lon_to_mx(p.lon) + ctx.mx_offset;
                    auto cy = lat_to_my(p.lat);
                    auto r = ctx.req.half_extent_y * SYMBOL_RADIUS_PJA;
                    emit_pja_point_icon(ctx.poly[layer_pja], cx, cy, r,
                                         ctx.styles.pja_point_style());
                }
            }
        }

        void maa_type::build(const build_context& ctx) const
        {
            auto area_vis = ctx.styles.maa_area_visible(ctx.req.zoom);
            auto point_vis = ctx.styles.maa_point_visible(ctx.req.zoom);
            if(!area_vis && !point_vis) return;
            if(!ctx.req.altitude.low_enabled()) return;

            const auto& maas = ctx.db.query_maas(
                request_bbox(ctx.req));

            for(const auto& m : maas)
            {
                auto is_area = false;
                if(!m.shape.empty() && area_vis)
                {
                    auto ls = to_line_style(ctx.styles.maa_area_style());
                    add_polyline(ctx.poly[layer_maa], m.shape, ls, ctx.mx_offset);
                    is_area = true;
                }
                else if(m.radius_nm > 0.0 && area_vis)
                {
                    auto ls = to_line_style(ctx.styles.maa_area_style());
                    add_geodesic_circle(ctx.poly[layer_maa],
                        m.lat, m.lon, m.radius_nm,
                        ls, ctx.mx_offset);
                    is_area = true;
                }
                else if(m.lat != 0.0 && point_vis)
                {
                    auto cx = lon_to_mx(m.lon) + ctx.mx_offset;
                    auto cy = lat_to_my(m.lat);
                    auto r = ctx.req.half_extent_y * SYMBOL_RADIUS_AIRPORT;
                    emit_maa_point_icon(ctx.poly[layer_maa], cx, cy, r, m,
                                         ctx.styles.maa_point_style());
                }

                if(is_area && ctx.req.zoom >= AIRSPACE_LABEL_MIN_ZOOM)
                {
                    auto [mx, my] = !m.shape.empty()
                        ? polygon_label_pos(m.shape, ctx.mx_offset)
                        : circle_label_pos(m.lat, m.lon, m.radius_nm,
                                           ctx.mx_offset);
                    std::string upper = format_altitude(m.max_alt_ft,
                        m.max_alt_ref.empty() ? "MSL" : m.max_alt_ref);
                    std::string lower = format_altitude(m.min_alt_ft,
                        m.min_alt_ref.empty() ? "SFC" : m.min_alt_ref);
                    if(upper.empty()) upper = "UNL";
                    emit_airspace_label(ctx, "MAA", mx, my, layer_maa,
                        upper, lower, ctx.styles.maa_area_style());
                }
            }
        }

        void tfr_type::build(const build_context& ctx) const
        {
            if(!ctx.styles.tfr_visible(ctx.req.zoom)) return;
            if(!ctx.req.altitude.any()) return;
            auto bbox = request_bbox(ctx.req);
            const auto& segs = ctx.db.query_tfr_segments(bbox);
            auto ls = to_line_style(ctx.styles.tfr_style());
            for(const auto& seg : segs)
            {
                if(!sua_altitude_visible(ctx.req.altitude,
                                          seg.upper_ft_val, seg.upper_ft_ref,
                                          seg.lower_ft_val, seg.lower_ft_ref))
                    continue;
                add_polyline(ctx.poly[layer_tfr], seg.points, ls, ctx.mx_offset);
            }

            if(ctx.req.zoom < AIRSPACE_LABEL_MIN_ZOOM) return;
            const auto& tfrs = ctx.db.query_tfr(bbox);
            const auto& fs = ctx.styles.tfr_style();
            for(const auto& t : tfrs)
            {
                for(const auto& area : t.areas)
                {
                    if(!sua_altitude_visible(ctx.req.altitude,
                                              area.upper_ft_val, area.upper_ft_ref,
                                              area.lower_ft_val, area.lower_ft_ref))
                        continue;
                    if(area.points.empty()) continue;
                    auto [cmx, cmy] = polygon_label_pos(area.points, ctx.mx_offset);
                    emit_airspace_label(ctx, "TFR", cmx, cmy, layer_tfr,
                        format_altitude(area.upper_ft_val, area.upper_ft_ref),
                        format_altitude(area.lower_ft_val, area.lower_ft_ref),
                        fs);
                }
            }
        }

        void adiz_type::build(const build_context& ctx) const
        {
            if(!ctx.styles.adiz_visible(ctx.req.zoom)) return;
            auto bbox = request_bbox(ctx.req);
            const auto& segs = ctx.db.query_adiz_segments(bbox);
            auto ls = to_line_style(ctx.styles.adiz_style());
            for(const auto& seg : segs)
                add_polyline(ctx.poly[layer_adiz], seg.points, ls, ctx.mx_offset);

            if(ctx.req.zoom < AIRSPACE_LABEL_MIN_ZOOM) return;
            const auto& adizs = ctx.db.query_adiz(bbox);
            for(const auto& a : adizs)
            {
                if(a.parts.empty() || a.parts[0].empty()) continue;
                auto [cmx, cmy] = polygon_label_pos(a.parts[0], ctx.mx_offset);
                emit_airspace_label(ctx, "ADIZ", cmx, cmy, layer_adiz,
                    "UNL", "SFC", ctx.styles.adiz_style());
            }
        }

        void artcc_type::build(const build_context& ctx) const
        {
            if(!ctx.styles.any_artcc_visible(ctx.req.zoom)) return;
            if(!ctx.req.altitude.any()) return;
            auto bbox = request_bbox(ctx.req);
            const auto& segs = ctx.db.query_artcc_segments(bbox);
            for(const auto& seg : segs)
            {
                if(!altitude_filter_allows(ctx.req.altitude, artcc_bands(seg.altitude)))
                    continue;
                ctx.styles.for_each_visible_artcc_style(
                    seg.altitude, seg.type, ctx.req.zoom,
                    [&](const feature_style& fs) {
                        auto ls = to_line_style(fs);
                        add_polyline(ctx.poly[layer_artcc], seg.points, ls, ctx.mx_offset);
                    });
            }

            if(ctx.req.zoom < AIRSPACE_LABEL_MIN_ZOOM) return;
            const auto& artccs = ctx.db.query_artcc(bbox);
            for(const auto& a : artccs)
            {
                if(!ctx.styles.artcc_visible(a.altitude, a.type, ctx.req.zoom)) continue;
                if(!altitude_filter_allows(ctx.req.altitude, artcc_bands(a.altitude)))
                    continue;
                if(a.points.empty()) continue;
                auto [cmx, cmy] = polygon_label_pos(a.points, ctx.mx_offset);
                emit_airspace_label(ctx, "ARTCC", cmx, cmy, layer_artcc,
                    a.location_id, a.altitude,
                    ctx.styles.artcc_style(a.altitude, a.type));
            }
        }

        void obstacle_type::build(const build_context& ctx) const
        {
            if(!ctx.styles.any_obstacle_visible(ctx.req.zoom)) return;
            if(!ctx.req.altitude.low_enabled()) return;
            const auto& obstacles = ctx.db.query_obstacles(
                request_bbox(ctx.req));
            auto radius = ctx.req.half_extent_y * SYMBOL_RADIUS_OBSTACLE;

            for(const auto& obs : obstacles)
            {
                if(!ctx.styles.obstacle_visible(obs.agl_ht, ctx.req.zoom)) continue;
                const auto& fs = ctx.styles.obstacle_style(obs.agl_ht);
                auto cx = lon_to_mx(obs.lon) + ctx.mx_offset;
                auto cy = lat_to_my(obs.lat);
                emit_obstacle_icon(ctx.poly[layer_obstacles], cx, cy, radius, obs, fs);
            }
        }

        void airspace_type::build(const build_context& ctx) const
        {
            if(!ctx.styles.any_airspace_visible(ctx.req.zoom)) return;
            if(!ctx.req.altitude.low_enabled()) return;
            auto bbox = request_bbox(ctx.req);
            const auto& segs = ctx.db.query_class_airspace_segments(
                bbox, ctx.styles.visible_airspace_values(ctx.req.zoom));
            for(const auto& seg : segs)
            {
                if(!ctx.styles.airspace_visible(seg.airspace_class, seg.local_type,
                                                 ctx.req.zoom)) continue;
                auto ls = to_line_style(ctx.styles.airspace_style(
                    seg.airspace_class, seg.local_type));
                add_polyline(ctx.poly[layer_airspace], seg.points, ls, ctx.mx_offset);
            }

            if(ctx.req.zoom < AIRSPACE_LABEL_MIN_ZOOM) return;
            const auto& arsp = ctx.db.query_class_airspace(bbox);
            for(const auto& a : arsp)
            {
                if(!ctx.styles.airspace_visible(a.airspace_class, a.local_type,
                                                 ctx.req.zoom)) continue;
                if(a.parts.empty() || a.parts[0].points.empty()) continue;
                auto [cmx, cmy] = polygon_label_pos(a.parts[0].points,
                                                    ctx.mx_offset);
                const auto& fs = ctx.styles.airspace_style(
                    a.airspace_class, a.local_type);
                // Undefined upper (class E) defaults to 18000 MSL (FL180)
                std::string upper = a.upper_ref.empty()
                    ? format_altitude(18000, "MSL")
                    : format_altitude(a.upper_ft, a.upper_ref);
                std::string lower = a.lower_ref == "SFC" || a.lower_ref.empty()
                    ? "SFC"
                    : format_altitude(a.lower_ft, a.lower_ref);
                emit_airspace_label(ctx, a.airspace_class,
                    cmx, cmy, layer_airspace, upper, lower, fs);
            }
        }

        void comm_outlet_type::build(const build_context& ctx) const
        {
            if(!ctx.styles.rco_visible(ctx.req.zoom)) return;
            if(!ctx.req.altitude.low_enabled()) return;
            auto radius = ctx.req.half_extent_y * SYMBOL_RADIUS_COMM;
            auto ls = to_line_style(ctx.styles.rco_style());
            for(const auto& f : ctx.db.query_comm_outlets(request_bbox(ctx.req)))
            {
                auto cx = lon_to_mx(f.lon) + ctx.mx_offset;
                auto cy = lat_to_my(f.lat);
                emit_comm_icon(ctx.poly[layer_rco], cx, cy, radius, ls);
            }
        }

        void awos_type::build(const build_context& ctx) const
        {
            if(!ctx.styles.awos_visible(ctx.req.zoom)) return;
            if(!ctx.req.altitude.low_enabled()) return;
            auto radius = ctx.req.half_extent_y * SYMBOL_RADIUS_COMM;
            auto ls = to_line_style(ctx.styles.awos_style());
            for(const auto& f : ctx.db.query_awos(request_bbox(ctx.req)))
            {
                auto cx = lon_to_mx(f.lon) + ctx.mx_offset;
                auto cy = lat_to_my(f.lat);
                emit_comm_icon(ctx.poly[layer_awos], cx, cy, radius, ls);
            }
        }

        // -- build_selection() bodies ---------------------------------------

        // Halo scale used for the filled-disc glow behind a selected point
        // feature: airport_outer (1.2) × 1.5.
        constexpr auto HALO_SCALE = 1.8;

        void emit_halo(polyline_data& out, double cx, double cy,
                        double r_base, double pixels_per_world)
        {
            auto halo_r = r_base * HALO_SCALE;
            auto fill_px = static_cast<float>(halo_r * pixels_per_world);
            auto halo_ls = line_style{fill_px, 0, 0, 0, 1, 1, 1, 1, 0};
            add_circle_to(out, cx, cy, halo_r * 0.5, halo_ls);
        }

        // Convert a polygon ring to Mercator vec2s, interpolating
        // long edges along the great circle.
        void ring_to_mercator(const std::vector<airspace_point>& pts,
                                std::vector<glm::vec2>& out_ring)
        {
            out_ring.clear();
            if(pts.empty()) return;
            out_ring.reserve(pts.size());
            out_ring.emplace_back(
                static_cast<float>(lon_to_mx(pts[0].lon)),
                static_cast<float>(lat_to_my(pts[0].lat)));
            for(std::size_t i = 1; i < pts.size(); i++)
            {
                auto seg = geodesic_interpolate(
                    pts[i - 1].lat, pts[i - 1].lon,
                    pts[i].lat, pts[i].lon);
                for(std::size_t j = 1; j < seg.size(); j++)
                    out_ring.emplace_back(
                        static_cast<float>(lon_to_mx(seg[j].lon)),
                        static_cast<float>(lat_to_my(seg[j].lat)));
            }
        }

        void circle_to_ring(double center_lon, double center_lat,
                              double radius_nm,
                              std::vector<glm::vec2>& out_ring)
        {
            auto pts = geodesic_circle(center_lat, center_lon, radius_nm);
            out_ring.clear();
            out_ring.reserve(pts.size());
            for(const auto& p : pts)
                out_ring.emplace_back(
                    static_cast<float>(lon_to_mx(p.lon)),
                    static_cast<float>(lat_to_my(p.lat)));
        }

        // Emit a white widened outline on top of an area feature. Same
        // line width as the normal outline plus 2×border + 2px padding.
        void emit_selection_boundary(polyline_data& out,
                                       const std::vector<glm::vec2>& ring,
                                       const line_style& base)
        {
            if(ring.size() < 2) return;
            auto ls = base;
            ls.r = ls.g = ls.b = 1.0F; ls.a = 1.0F;
            ls.line_width = ls.line_width + 2.0F * ls.border_width + 2.0F;
            ls.border_width = 0; ls.dash_length = 0; ls.gap_length = 0;
            out.polylines.push_back(ring);
            out.styles.push_back(ls);
        }

        // Convert an airway/MTR line_style into its widened "selection"
        // variant used for highlighting a full route.
        line_style selection_line_style(const feature_style& fs)
        {
            auto ls = to_line_style(fs);
            ls.r = ls.g = ls.b = 1.0F; ls.a = 1.0F;
            ls.line_width = ls.line_width + 2.0F * ls.border_width + 2.0F;
            ls.border_width = 0; ls.dash_length = 0; ls.gap_length = 0;
            return ls;
        }

        // Common setup for a point-feature selection overlay: compute the
        // center + halo + icon scaling parameters shared by airport, navaid,
        // fix, obstacle, comm, awos overrides.
        struct point_selection_geom
        {
            double cx;
            double cy;
            double r_base;
            double pixels_per_world;
        };

        template<typename T>
        point_selection_geom point_selection_geom_for(const T& v,
                                                        const feature_build_request& req)
        {
            return {
                lon_to_mx(v.lon),
                lat_to_my(v.lat),
                req.half_extent_y * SYMBOL_RADIUS_AIRPORT,
                req.viewport_height / (2.0 * req.half_extent_y)
            };
        }

        void airport_type::build_selection(const build_context& ctx, const feature& f,
                                             polyline_data& out, polygon_fill_data& /*fill*/) const
        {
            const auto& v = std::get<airport>(f);
            auto g = point_selection_geom_for(v, ctx.req);
            emit_halo(out, g.cx, g.cy, g.r_base, g.pixels_per_world);
            emit_airport_icon(out, g.cx, g.cy, g.r_base, g.pixels_per_world,
                                v, ctx.styles.airport_style(v));
        }

        void navaid_type::build_selection(const build_context& ctx, const feature& f,
                                            polyline_data& out, polygon_fill_data& /*fill*/) const
        {
            const auto& v = std::get<navaid>(f);
            auto g = point_selection_geom_for(v, ctx.req);
            emit_halo(out, g.cx, g.cy, g.r_base, g.pixels_per_world);
            emit_navaid_icon(out, g.cx, g.cy, g.r_base, v,
                               ctx.styles.navaid_style(v.nav_type));
        }

        void fix_type::build_selection(const build_context& ctx, const feature& f,
                                         polyline_data& out, polygon_fill_data& /*fill*/) const
        {
            const auto& v = std::get<fix>(f);
            auto g = point_selection_geom_for(v, ctx.req);
            emit_halo(out, g.cx, g.cy, g.r_base, g.pixels_per_world);
            emit_fix_icon(out, g.cx, g.cy, g.r_base, v, ctx.styles.fix_style(v.use_code));
        }

        void obstacle_type::build_selection(const build_context& ctx, const feature& f,
                                              polyline_data& out, polygon_fill_data& /*fill*/) const
        {
            const auto& v = std::get<obstacle>(f);
            auto g = point_selection_geom_for(v, ctx.req);
            emit_halo(out, g.cx, g.cy, g.r_base, g.pixels_per_world);
            emit_obstacle_icon(out, g.cx, g.cy, g.r_base, v, ctx.styles.obstacle_style(v.agl_ht));
        }

        void awos_type::build_selection(const build_context& ctx, const feature& f,
                                          polyline_data& out, polygon_fill_data& /*fill*/) const
        {
            const auto& v = std::get<awos>(f);
            auto g = point_selection_geom_for(v, ctx.req);
            emit_halo(out, g.cx, g.cy, g.r_base, g.pixels_per_world);
            emit_comm_icon(out, g.cx, g.cy, g.r_base, to_line_style(ctx.styles.awos_style()));
        }

        void comm_outlet_type::build_selection(const build_context& ctx, const feature& f,
                                                  polyline_data& out, polygon_fill_data& /*fill*/) const
        {
            const auto& v = std::get<comm_outlet>(f);
            auto g = point_selection_geom_for(v, ctx.req);
            emit_halo(out, g.cx, g.cy, g.r_base, g.pixels_per_world);
            emit_comm_icon(out, g.cx, g.cy, g.r_base, to_line_style(ctx.styles.rco_style()));
        }

        void runway_type::build_selection(const build_context& /*ctx*/, const feature& /*f*/,
                                            polyline_data& /*out*/, polygon_fill_data& /*fill*/) const
        {
            // Runways have no selection overlay in the original code.
        }

        void pja_type::build_selection(const build_context& ctx, const feature& f,
                                         polyline_data& out, polygon_fill_data& fill_out) const
        {
            const auto& v = std::get<pja>(f);
            if(v.radius_nm > 0.0)
            {
                auto fs = ctx.styles.pja_area_style();
                auto base = to_line_style(fs);
                glm::vec4 fill_color{fs.r, fs.g, fs.b, 0.25F};
                std::vector<glm::vec2> ring;
                circle_to_ring(v.lon, v.lat, v.radius_nm, ring);
                emit_selection_boundary(out, ring, base);
                triangulate_polygon(ring, {}, fill_color, fill_out);
            }
            else
            {
                auto g = point_selection_geom_for(v, ctx.req);
                emit_halo(out, g.cx, g.cy, g.r_base, g.pixels_per_world);
                emit_pja_point_icon(out, g.cx, g.cy, g.r_base,
                                     ctx.styles.pja_point_style());
            }
        }

        void maa_type::build_selection(const build_context& ctx, const feature& f,
                                         polyline_data& out, polygon_fill_data& fill_out) const
        {
            const auto& v = std::get<maa>(f);
            if(!v.shape.empty() || v.radius_nm > 0.0)
            {
                auto fs = ctx.styles.maa_area_style();
                auto base = to_line_style(fs);
                glm::vec4 fill_color{fs.r, fs.g, fs.b, 0.25F};
                std::vector<glm::vec2> ring;
                if(!v.shape.empty()) ring_to_mercator(v.shape, ring);
                else                  circle_to_ring(v.lon, v.lat, v.radius_nm, ring);
                emit_selection_boundary(out, ring, base);
                triangulate_polygon(ring, {}, fill_color, fill_out);
            }
            else
            {
                auto g = point_selection_geom_for(v, ctx.req);
                emit_halo(out, g.cx, g.cy, g.r_base, g.pixels_per_world);
                emit_maa_point_icon(out, g.cx, g.cy, g.r_base, v,
                                     ctx.styles.maa_point_style());
            }
        }

        void airway_type::build_selection(const build_context& ctx, const feature& f,
                                            polyline_data& out, polygon_fill_data& /*fill*/) const
        {
            const auto& v = std::get<airway_segment>(f);
            auto ls = selection_line_style(ctx.styles.airway_style(v.awy_id));
            for(const auto& seg : ctx.db.query_airway_by_id(v.awy_id))
            {
                if(seg.gap_flag == "Y") continue;
                auto arc = geodesic_interpolate(
                    seg.from_lat, seg.from_lon, seg.to_lat, seg.to_lon);
                std::vector<glm::vec2> polyline;
                polyline.reserve(arc.size());
                for(const auto& p : arc)
                    polyline.emplace_back(
                        static_cast<float>(lon_to_mx(p.lon)),
                        static_cast<float>(lat_to_my(p.lat)));
                out.polylines.push_back(std::move(polyline));
                out.styles.push_back(ls);
            }
        }

        void mtr_type::build_selection(const build_context& ctx, const feature& f,
                                         polyline_data& out, polygon_fill_data& /*fill*/) const
        {
            const auto& v = std::get<mtr_segment>(f);
            auto ls = selection_line_style(ctx.styles.mtr_style());
            for(const auto& seg : ctx.db.query_mtr_by_id(v.mtr_id))
            {
                auto arc = geodesic_interpolate(
                    seg.from_lat, seg.from_lon, seg.to_lat, seg.to_lon);
                std::vector<glm::vec2> polyline;
                polyline.reserve(arc.size());
                for(const auto& p : arc)
                    polyline.emplace_back(
                        static_cast<float>(lon_to_mx(p.lon)),
                        static_cast<float>(lat_to_my(p.lat)));
                out.polylines.push_back(std::move(polyline));
                out.styles.push_back(ls);
            }
        }

        void airspace_type::build_selection(const build_context& ctx, const feature& f,
                                              polyline_data& out, polygon_fill_data& fill_out) const
        {
            const auto& v = std::get<class_airspace>(f);
            auto fs = ctx.styles.airspace_style(v.airspace_class, v.local_type);
            auto base = to_line_style(fs);
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
            for(const auto& part : v.parts)
            {
                ring_to_mercator(part.points, ring);
                emit_selection_boundary(out, ring, base);
                if(part.is_hole) holes.push_back(ring);
                else { flush(); outer = ring; }
            }
            flush();
        }

        void sua_type::build_selection(const build_context& ctx, const feature& f,
                                         polyline_data& out, polygon_fill_data& fill_out) const
        {
            const auto& v = std::get<sua>(f);
            auto fs = ctx.styles.sua_style(v.sua_type);
            auto base = to_line_style(fs);
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
            for(const auto& stratum : v.strata)
            {
                for(const auto& part : stratum.parts)
                {
                    if(part.is_circle)
                        circle_to_ring(part.circle_lon, part.circle_lat,
                                        part.circle_radius_nm, ring);
                    else
                        ring_to_mercator(part.points, ring);
                    emit_selection_boundary(out, ring, base);
                    if(part.is_hole) holes.push_back(ring);
                    else { flush(); outer = ring; }
                }
                flush();
            }
        }

        void artcc_type::build_selection(const build_context& ctx, const feature& f,
                                           polyline_data& out, polygon_fill_data& fill_out) const
        {
            const auto& v = std::get<artcc>(f);
            auto fs = ctx.styles.artcc_style(v.altitude, v.type);
            auto base = to_line_style(fs);
            glm::vec4 fill_color{fs.r, fs.g, fs.b, 0.25F};
            std::vector<glm::vec2> ring;
            ring_to_mercator(v.points, ring);
            emit_selection_boundary(out, ring, base);
            triangulate_polygon(ring, {}, fill_color, fill_out);
        }

        void tfr_type::build_selection(const build_context& ctx, const feature& f,
                                         polyline_data& out, polygon_fill_data& fill_out) const
        {
            const auto& v = std::get<tfr>(f);
            auto fs = ctx.styles.tfr_style();
            auto base = to_line_style(fs);
            glm::vec4 fill_color{fs.r, fs.g, fs.b, 0.25F};
            std::vector<glm::vec2> ring;
            for(const auto& area : v.areas)
            {
                ring_to_mercator(area.points, ring);
                emit_selection_boundary(out, ring, base);
                triangulate_polygon(ring, {}, fill_color, fill_out);
            }
        }

        void adiz_type::build_selection(const build_context& ctx, const feature& f,
                                          polyline_data& out, polygon_fill_data& fill_out) const
        {
            const auto& v = std::get<adiz>(f);
            auto fs = ctx.styles.adiz_style();
            auto base = to_line_style(fs);
            glm::vec4 fill_color{fs.r, fs.g, fs.b, 0.25F};
            std::vector<glm::vec2> ring;
            for(const auto& part : v.parts)
            {
                ring_to_mercator(part, ring);
                emit_selection_boundary(out, ring, base);
                triangulate_polygon(ring, {}, fill_color, fill_out);
            }
        }

        // PJA layer anchors to its center (when known) rather than the click.
        class pja_type_anchored : public pja_type {
        public:
            std::pair<double, double>
            anchor_lonlat(const feature& f,
                           double /*click_lon*/, double /*click_lat*/) const override
            {
                const auto& v = std::get<pja>(f);
                return {v.lon, v.lat};
            }
        };

        // MAA anchors to its center when the MAA is point/circle-defined;
        // shape-only MAAs fall back to the click point.
        class maa_type_anchored : public maa_type {
        public:
            std::pair<double, double>
            anchor_lonlat(const feature& f,
                           double click_lon, double click_lat) const override
            {
                const auto& v = std::get<maa>(f);
                if(v.shape.empty()) return {v.lon, v.lat};
                return {click_lon, click_lat};
            }
        };
    }

    std::vector<std::unique_ptr<feature_type>> make_feature_types()
    {
        // Order controls z-priority of pick results and checkbox order.
        std::vector<std::unique_ptr<feature_type>> v;
        v.reserve(16);
        // Order also determines build dependency order: navaid populates
        // navaid_positions (used by airway clearance), airway populates
        // airway_waypoints (used by fix on-airway test). So: navaid ->
        // airway -> fix.
        v.push_back(std::make_unique<airport_type>());
        v.push_back(std::make_unique<runway_type>());
        v.push_back(std::make_unique<navaid_type>());
        v.push_back(std::make_unique<airway_type>());
        v.push_back(std::make_unique<fix_type>());
        v.push_back(std::make_unique<mtr_type>());
        v.push_back(std::make_unique<pja_type_anchored>());
        v.push_back(std::make_unique<maa_type_anchored>());
        v.push_back(std::make_unique<airspace_type>());
        v.push_back(std::make_unique<sua_type>());
        v.push_back(std::make_unique<tfr_type>());
        v.push_back(std::make_unique<adiz_type>());
        v.push_back(std::make_unique<artcc_type>());
        v.push_back(std::make_unique<obstacle_type>());
        v.push_back(std::make_unique<comm_outlet_type>());
        v.push_back(std::make_unique<awos_type>());
        return v;
    }

    const feature_type& find_feature_type(
        const std::vector<std::unique_ptr<feature_type>>& feature_types,
        const feature& f)
    {
        for(const auto& obj : feature_types)
            if(obj->owns(f))
                return *obj;
        throw std::logic_error("find_feature_type: no feature_type owns this feature variant");
    }
}
