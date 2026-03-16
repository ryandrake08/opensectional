#include "chart_style.hpp"
#include "ini_config.hpp"
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
        "airport_hard_long", "airport_hard_short", "airport_other",
        "airport_towered", "airport_untowered", "airport_heliport",
        "navaid_vor", "navaid_ndb",
        "fix_wp", "fix_rp", "fix_vfr",
        "obstacle_1000ft", "obstacle_200ft", "obstacle_low",
        "airway_v", "airway_t", "airway_br", "airway_tk",
        "airway_j", "airway_q", "airway_ar", "airway_rte", "airway_h",
        "airway_m", "airway_l", "airway_y", "airway_w", "airway_n",
        "airway_r", "airway_a", "airway_g", "airway_b",
        "airspace_b", "airspace_c", "airspace_d", "airspace_e",
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

} // namespace nasrbrowse
