#include "chart_style.hpp"
#include "ini_config.hpp"
#include "nasr_database.hpp"
#include <fstream>
#include <stdexcept>

namespace nasrbrowse
{
    static std::string mode_suffix(chart_mode mode)
    {
        switch(mode)
        {
        case chart_mode::vfr: return "vfr";
        case chart_mode::ifr_low: return "ifr_low";
        case chart_mode::ifr_high: return "ifr_high";
        }
        return "vfr";
    }

    static double get_or(const ini_config& cfg, const std::string& key, double fallback)
    {
        return cfg.exists(key) ? cfg.get<double>(key) : fallback;
    }

    static float get_or_f(const ini_config& cfg, const std::string& key, float fallback)
    {
        return cfg.exists(key) ? cfg.get<float>(key) : fallback;
    }

    static feature_style load_style(const ini_config& ini, const std::string& prefix,
                                     const feature_style& fallback)
    {
        feature_style s = fallback;
        s.min_zoom     = get_or(ini, prefix + "min_zoom", s.min_zoom);
        s.max_zoom     = get_or(ini, prefix + "max_zoom", s.max_zoom);
        s.r            = get_or_f(ini, prefix + "r", s.r);
        s.g            = get_or_f(ini, prefix + "g", s.g);
        s.b            = get_or_f(ini, prefix + "b", s.b);
        s.a            = get_or_f(ini, prefix + "a", s.a);
        s.line_width   = get_or_f(ini, prefix + "line_width", s.line_width);
        s.border_width = get_or_f(ini, prefix + "border_width", s.border_width);
        s.dash_length  = get_or_f(ini, prefix + "dash_length", s.dash_length);
        s.gap_length   = get_or_f(ini, prefix + "gap_length", s.gap_length);
        return s;
    }

    // All known feature keys that the renderer may look up
    static const char* all_keys[] = {
        "airport_class_b", "airport_class_c", "airport_class_d",
        "airport_class_e", "airport_other",
        "airport_towered", "airport_untowered",
        "navaid_vor", "navaid_ndb",
        "mtr",
        "fix_airway", "fix_noairway",
        "fix_wp", "fix_rp", "fix_vfr", "fix_cn", "fix_mr", "fix_mw", "fix_nrs",
        "obstacle_1000ft", "obstacle_200ft", "obstacle_low",
        "airway_v", "airway_t", "airway_br", "airway_tk",
        "airway_j", "airway_q", "airway_ar", "airway_rte", "airway_h",
        "airway_m", "airway_l", "airway_y", "airway_w", "airway_n",
        "airway_r", "airway_a", "airway_g", "airway_b",
        "airspace_b", "airspace_c", "airspace_d",
        "airspace_e2", "airspace_e3", "airspace_e4",
        "airspace_e5", "airspace_e6", "airspace_e7",
        "artcc_low", "artcc_high", "artcc_oceanic",
        "pja_area", "pja_point",
        "maa_area", "maa_point",
        "adiz",
        "sua_prohibited", "sua_restricted", "sua_warning",
        "sua_alert", "sua_moa", "sua_nsa",
        "runway",
    };

    chart_style::chart_style(const std::string& ini_path, chart_mode mode)
        : fallback{0.0, 99.0, 1.0F, 0.0F, 1.0F, 1.0F, 20.0F, 5.0F, 30.0F, 15.0F}
    {
        std::ifstream test(ini_path);
        if(!test.good())
        {
            throw std::runtime_error("Chart style INI not found: " + ini_path);
        }
        test.close();

        ini_config ini(ini_path);
        std::string suffix = mode_suffix(mode);

        for(const char* key : all_keys)
        {
            std::string prefix = std::string(key) + "." + suffix + ".";
            styles[key] = load_style(ini, prefix, fallback);
        }
    }

    const feature_style& chart_style::get(const std::string& key) const
    {
        auto it = styles.find(key);
        if(it != styles.end())
        {
            return it->second;
        }
        return fallback;
    }

    bool chart_style::visible(const std::string& key, double zoom) const
    {
        const auto& s = get(key);
        return zoom >= s.min_zoom && zoom <= s.max_zoom;
    }

    // Key-mapping helpers (map feature attributes to INI key names)

    static const char* airport_zoom_key(const airport& apt)
    {
        if(apt.airspace_class == "B") return "airport_class_b";
        if(apt.airspace_class == "C") return "airport_class_c";
        if(apt.airspace_class == "D") return "airport_class_d";
        if(apt.airspace_class == "E") return "airport_class_e";
        return "airport_other";
    }

    static const char* airport_color_key(const airport& apt)
    {
        if(apt.twr_type_code != "NON-ATCT") return "airport_towered";
        return "airport_untowered";
    }

    static const char* navaid_key(const std::string& nav_type)
    {
        return (nav_type == "NDB" || nav_type == "NDB/DME")
            ? "navaid_ndb" : "navaid_vor";
    }

    static const char* fix_zoom_key(bool on_airway)
    {
        return on_airway ? "fix_airway" : "fix_noairway";
    }

    static const char* fix_color_key(const std::string& use_code)
    {
        if(use_code == "RP") return "fix_rp";
        if(use_code == "VFR") return "fix_vfr";
        if(use_code == "CN") return "fix_cn";
        if(use_code == "MR") return "fix_mr";
        if(use_code == "MW") return "fix_mw";
        if(use_code == "NRS") return "fix_nrs";
        return "fix_wp";
    }

    static const char* obstacle_key(int agl_ht)
    {
        if(agl_ht >= 1000) return "obstacle_1000ft";
        if(agl_ht >= 200) return "obstacle_200ft";
        return "obstacle_low";
    }

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

    static const char* airspace_key(const std::string& cls, const std::string& local_type)
    {
        if(cls == "B") return "airspace_b";
        if(cls == "C") return "airspace_c";
        if(cls == "D") return "airspace_d";
        if(local_type == "CLASS_E2") return "airspace_e2";
        if(local_type == "CLASS_E3") return "airspace_e3";
        if(local_type == "CLASS_E4") return "airspace_e4";
        if(local_type == "CLASS_E5") return "airspace_e5";
        if(local_type == "CLASS_E6") return "airspace_e6";
        if(local_type == "CLASS_E7") return "airspace_e7";
        return "airspace_e2";
    }

    static const char* sua_key(const std::string& sua_type)
    {
        if(sua_type == "RA") return "sua_restricted";
        if(sua_type == "PA") return "sua_prohibited";
        if(sua_type == "WA") return "sua_warning";
        if(sua_type == "AA") return "sua_alert";
        if(sua_type == "NSA") return "sua_nsa";
        return "sua_moa";
    }

    static const char* artcc_key(const std::string& altitude)
    {
        if(altitude == "HIGH") return "artcc_high";
        if(altitude == "UNLIMITED") return "artcc_oceanic";
        return "artcc_low";
    }

    // Airport
    bool chart_style::airport_visible(const airport& apt, double zoom) const
    { return visible(airport_zoom_key(apt), zoom); }
    const feature_style& chart_style::airport_style(const airport& apt) const
    { return get(airport_color_key(apt)); }

    // Navaid
    bool chart_style::navaid_visible(const std::string& nav_type, double zoom) const
    { return visible(navaid_key(nav_type), zoom); }
    const feature_style& chart_style::navaid_style(const std::string& nav_type) const
    { return get(navaid_key(nav_type)); }

    // Fix
    bool chart_style::fix_visible(bool on_airway, double zoom) const
    { return visible(fix_zoom_key(on_airway), zoom); }
    const feature_style& chart_style::fix_style(const std::string& use_code) const
    { return get(fix_color_key(use_code)); }

    // Obstacle
    bool chart_style::obstacle_visible(int agl_ht, double zoom) const
    { return visible(obstacle_key(agl_ht), zoom); }
    const feature_style& chart_style::obstacle_style(int agl_ht) const
    { return get(obstacle_key(agl_ht)); }

    // Airway
    bool chart_style::airway_visible(const std::string& awy_id, double zoom) const
    { return visible(airway_key(awy_id), zoom); }
    const feature_style& chart_style::airway_style(const std::string& awy_id) const
    { return get(airway_key(awy_id)); }

    // MTR
    bool chart_style::mtr_visible(double zoom) const
    { return visible("mtr", zoom); }
    const feature_style& chart_style::mtr_style() const
    { return get("mtr"); }

    // Runway
    bool chart_style::runway_visible(double zoom) const
    { return visible("runway", zoom); }
    const feature_style& chart_style::runway_style() const
    { return get("runway"); }

    // Class airspace
    bool chart_style::airspace_visible(const std::string& airspace_class,
                                        const std::string& local_type, double zoom) const
    { return visible(airspace_key(airspace_class, local_type), zoom); }
    const feature_style& chart_style::airspace_style(const std::string& airspace_class,
                                                      const std::string& local_type) const
    { return get(airspace_key(airspace_class, local_type)); }

    // SUA
    bool chart_style::sua_visible(const std::string& sua_type, double zoom) const
    { return visible(sua_key(sua_type), zoom); }
    const feature_style& chart_style::sua_style(const std::string& sua_type) const
    { return get(sua_key(sua_type)); }

    // ARTCC
    bool chart_style::artcc_visible(const std::string& altitude, double zoom) const
    { return visible(artcc_key(altitude), zoom); }
    const feature_style& chart_style::artcc_style(const std::string& altitude) const
    { return get(artcc_key(altitude)); }

    // ADIZ
    bool chart_style::adiz_visible(double zoom) const
    { return visible("adiz", zoom); }
    const feature_style& chart_style::adiz_style() const
    { return get("adiz"); }

    // PJA
    bool chart_style::pja_area_visible(double zoom) const
    { return visible("pja_area", zoom); }
    bool chart_style::pja_point_visible(double zoom) const
    { return visible("pja_point", zoom); }
    const feature_style& chart_style::pja_area_style() const
    { return get("pja_area"); }
    const feature_style& chart_style::pja_point_style() const
    { return get("pja_point"); }

    // MAA
    bool chart_style::maa_area_visible(double zoom) const
    { return visible("maa_area", zoom); }
    bool chart_style::maa_point_visible(double zoom) const
    { return visible("maa_point", zoom); }
    const feature_style& chart_style::maa_area_style() const
    { return get("maa_area"); }
    const feature_style& chart_style::maa_point_style() const
    { return get("maa_point"); }

} // namespace nasrbrowse
