#include "feature_type.hpp"

#include "altitude_filter.hpp"
#include "chart_style.hpp"
#include "ui_overlay.hpp"  // for the layer enum

#include <cmath>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <variant>

namespace nasrbrowse
{
    namespace
    {
        // -- Geometry picking helpers -------------------------------------------

        bool point_in_ring(double px, double py,
                        const std::vector<airspace_point>& ring)
        {
            bool inside = false;
            for(size_t i = 0, j = ring.size() - 1; i < ring.size(); j = i++)
            {
                double yi = ring[i].lat, yj = ring[j].lat;
                double xi = ring[i].lon, xj = ring[j].lon;
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
            bool inside = false;
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
            double dlat = (py - cy) * 60.0;
            double dlon = (px - cx) * 60.0 * std::cos(cy * M_PI / 180.0);
            return (dlat * dlat + dlon * dlon) <= (radius_nm * radius_nm);
        }

        double point_to_segment_nm(double px, double py,
                                    double ax, double ay, double bx, double by)
        {
            double cos_lat = std::cos(py * M_PI / 180.0);
            double ax_nm = (ax - px) * 60.0 * cos_lat;
            double ay_nm = (ay - py) * 60.0;
            double bx_nm = (bx - px) * 60.0 * cos_lat;
            double by_nm = (by - py) * 60.0;

            double dx = bx_nm - ax_nm;
            double dy = by_nm - ay_nm;
            double len2 = dx * dx + dy * dy;
            double t = 0;
            if(len2 > 0)
            {
                t = -(ax_nm * dx + ay_nm * dy) / len2;
                if(t < 0) t = 0;
                else if(t > 1) t = 1;
            }
            double cx = ax_nm + t * dx;
            double cy = ay_nm + t * dy;
            return std::sqrt(cx * cx + cy * cy);
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
            kv_list info_kv(const feature& f) const override;

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
                bool keep = (nav.is_low && ctx.vis.altitude.show_low)
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
                bool keep = (f.is_low && ctx.vis.altitude.show_low)
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
                    bool inside = false;
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
                if(!ctx.styles.artcc_visible(a.altitude, ctx.zoom)) continue;
                if(!altitude_filter_allows(ctx.vis.altitude, artcc_bands(a.altitude)))
                    continue;
                if(point_in_ring(ctx.click_lon, ctx.click_lat, a.points))
                    out.push_back(a);
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
            bool area_vis = ctx.styles.pja_area_visible(ctx.zoom);
            bool point_vis = ctx.styles.pja_point_visible(ctx.zoom);
            if(!area_vis && !point_vis) return;
            for(const auto& p : ctx.db.query_pjas(ctx.pick_box))
            {
                int upper = p.max_altitude_ft_msl > 0 ? p.max_altitude_ft_msl
                                                        : altitude_filter::UNLIMITED_FT;
                if(!ctx.vis.altitude.overlaps(0, upper)) continue;
                bool is_point = (p.radius_nm <= 0);
                if(is_point ? !point_vis : !area_vis) continue;
                bool hit = is_point
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
            bool area_vis = ctx.styles.maa_area_visible(ctx.zoom);
            bool point_vis = ctx.styles.maa_point_visible(ctx.zoom);
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
                    bool is_area = m.shape.empty();
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
                double d = point_to_segment_nm(ctx.click_lon, ctx.click_lat,
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
                double d = point_to_segment_nm(ctx.click_lon, ctx.click_lat,
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
                double d = point_to_segment_nm(ctx.click_lon, ctx.click_lat,
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
                {"Upper",      v.upper_val.empty() && v.upper_desc.empty() ? std::string() : v.upper_desc + " " + v.upper_val},
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
                if(event.empty()) return std::string();
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
                std::string start = fmt_endpoint(sc.start_time, sc.start_event, sc.start_event_offset);
                std::string end = fmt_endpoint(sc.end_time, sc.end_event, sc.end_event_offset);
                std::string val = day;
                if(!start.empty() || !end.empty()) val += " " + start + "-" + end;
                rows.push_back({"Schedule", val});
            }
            rows.push_back({"Legal note", v.legal_note});
            return rows;
        }

        kv_list artcc_type::info_kv(const feature& f) const
        {
            const auto& v = std::get<artcc>(f);
            return {
                {"Location ID",   v.location_id},
                {"Name",          v.name},
                {"Altitude band", v.altitude},
            };
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
                {"Min alt", v.min_alt},
                {"Max alt", v.max_alt},
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

        // PJA layer anchors to its center (when known) rather than the click.
        class pja_type_anchored : public pja_type {
        public:
            std::pair<double, double>
            anchor_lonlat(const feature& f,
                           double, double) const override
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
        v.reserve(15);
        v.push_back(std::make_unique<airport_type>());
        v.push_back(std::make_unique<runway_type>());
        v.push_back(std::make_unique<navaid_type>());
        v.push_back(std::make_unique<fix_type>());
        v.push_back(std::make_unique<airway_type>());
        v.push_back(std::make_unique<mtr_type>());
        v.push_back(std::make_unique<pja_type_anchored>());
        v.push_back(std::make_unique<maa_type_anchored>());
        v.push_back(std::make_unique<airspace_type>());
        v.push_back(std::make_unique<sua_type>());
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
