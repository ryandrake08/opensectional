#include "chart_style.hpp"
#include "ini_config.hpp"
#include "nasr_database.hpp"
#include <algorithm>
#include <array>
#include <fstream>
#include <stdexcept>
#include <unordered_map>

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

    // Parse a CSS color name or hex string (#RGB, #RRGGBB, #RRGGBBAA)
    static bool parse_css_color(const std::string& name, float& r, float& g, float& b, float& a)
    {
        auto to_float = [](int v) { return v / 255.0F; };

        // Resolve CSS named colors to their hex values
        // CSS Color Module Level 4: https://www.w3.org/TR/css-color-4/#named-colors
        static const std::unordered_map<std::string, std::string> names =
        {
            {"aliceblue", "#F0F8FF"}, {"antiquewhite", "#FAEBD7"},
            {"aqua", "#00FFFF"}, {"aquamarine", "#7FFFD4"},
            {"azure", "#F0FFFF"}, {"beige", "#F5F5DC"},
            {"bisque", "#FFE4C4"}, {"black", "#000000"},
            {"blanchedalmond", "#FFEBCD"}, {"blue", "#0000FF"},
            {"blueviolet", "#8A2BE2"}, {"brown", "#A52A2A"},
            {"burlywood", "#DEB887"}, {"cadetblue", "#5F9EA0"},
            {"chartreuse", "#7FFF00"}, {"chocolate", "#D2691E"},
            {"coral", "#FF7F50"}, {"cornflowerblue", "#6495ED"},
            {"cornsilk", "#FFF8DC"}, {"crimson", "#DC143C"},
            {"cyan", "#00FFFF"}, {"darkblue", "#00008B"},
            {"darkcyan", "#008B8B"}, {"darkgoldenrod", "#B8860B"},
            {"darkgray", "#A9A9A9"}, {"darkgreen", "#006400"},
            {"darkgrey", "#A9A9A9"}, {"darkkhaki", "#BDB76B"},
            {"darkmagenta", "#8B008B"}, {"darkolivegreen", "#556B2F"},
            {"darkorange", "#FF8C00"}, {"darkorchid", "#9932CC"},
            {"darkred", "#8B0000"}, {"darksalmon", "#E9967A"},
            {"darkseagreen", "#8FBC8F"}, {"darkslateblue", "#483D8B"},
            {"darkslategray", "#2F4F4F"}, {"darkslategrey", "#2F4F4F"},
            {"darkturquoise", "#00CED1"}, {"darkviolet", "#9400D3"},
            {"deeppink", "#FF1493"}, {"deepskyblue", "#00BFFF"},
            {"dimgray", "#696969"}, {"dimgrey", "#696969"},
            {"dodgerblue", "#1E90FF"}, {"firebrick", "#B22222"},
            {"floralwhite", "#FFFAF0"}, {"forestgreen", "#228B22"},
            {"fuchsia", "#FF00FF"}, {"gainsboro", "#DCDCDC"},
            {"ghostwhite", "#F8F8FF"}, {"gold", "#FFD700"},
            {"goldenrod", "#DAA520"}, {"gray", "#808080"},
            {"green", "#008000"}, {"greenyellow", "#ADFF2F"},
            {"grey", "#808080"}, {"honeydew", "#F0FFF0"},
            {"hotpink", "#FF69B4"}, {"indianred", "#CD5C5C"},
            {"indigo", "#4B0082"}, {"ivory", "#FFFFF0"},
            {"khaki", "#F0E68C"}, {"lavender", "#E6E6FA"},
            {"lavenderblush", "#FFF0F5"}, {"lawngreen", "#7CFC00"},
            {"lemonchiffon", "#FFFACD"}, {"lightblue", "#ADD8E6"},
            {"lightcoral", "#F08080"}, {"lightcyan", "#E0FFFF"},
            {"lightgoldenrodyellow", "#FAFAD2"}, {"lightgray", "#D3D3D3"},
            {"lightgreen", "#90EE90"}, {"lightgrey", "#D3D3D3"},
            {"lightpink", "#FFB6C1"}, {"lightsalmon", "#FFA07A"},
            {"lightseagreen", "#20B2AA"}, {"lightskyblue", "#87CEFA"},
            {"lightslategray", "#778899"}, {"lightslategrey", "#778899"},
            {"lightsteelblue", "#B0C4DE"}, {"lightyellow", "#FFFFE0"},
            {"lime", "#00FF00"}, {"limegreen", "#32CD32"},
            {"linen", "#FAF0E6"}, {"magenta", "#FF00FF"},
            {"maroon", "#800000"}, {"mediumaquamarine", "#66CDAA"},
            {"mediumblue", "#0000CD"}, {"mediumorchid", "#BA55D3"},
            {"mediumpurple", "#9370DB"}, {"mediumseagreen", "#3CB371"},
            {"mediumslateblue", "#7B68EE"}, {"mediumspringgreen", "#00FA9A"},
            {"mediumturquoise", "#48D1CC"}, {"mediumvioletred", "#C71585"},
            {"midnightblue", "#191970"}, {"mintcream", "#F5FFFA"},
            {"mistyrose", "#FFE4E1"}, {"moccasin", "#FFE4B5"},
            {"navajowhite", "#FFDEAD"}, {"navy", "#000080"},
            {"oldlace", "#FDF5E6"}, {"olive", "#808000"},
            {"olivedrab", "#6B8E23"}, {"orange", "#FFA500"},
            {"orangered", "#FF4500"}, {"orchid", "#DA70D6"},
            {"palegoldenrod", "#EEE8AA"}, {"palegreen", "#98FB98"},
            {"paleturquoise", "#AFEEEE"}, {"palevioletred", "#DB7093"},
            {"papayawhip", "#FFEFD5"}, {"peachpuff", "#FFDAB9"},
            {"peru", "#CD853F"}, {"pink", "#FFC0CB"},
            {"plum", "#DDA0DD"}, {"powderblue", "#B0E0E6"},
            {"purple", "#800080"}, {"rebeccapurple", "#663399"},
            {"red", "#FF0000"}, {"rosybrown", "#BC8F8F"},
            {"royalblue", "#4169E1"}, {"saddlebrown", "#8B4513"},
            {"salmon", "#FA8072"}, {"sandybrown", "#F4A460"},
            {"seagreen", "#2E8B57"}, {"seashell", "#FFF5EE"},
            {"sienna", "#A0522D"}, {"silver", "#C0C0C0"},
            {"skyblue", "#87CEEB"}, {"slateblue", "#6A5ACD"},
            {"slategray", "#708090"}, {"slategrey", "#708090"},
            {"snow", "#FFFAFA"}, {"springgreen", "#00FF7F"},
            {"steelblue", "#4682B4"}, {"tan", "#D2B48C"},
            {"teal", "#008080"}, {"thistle", "#D8BFD8"},
            {"tomato", "#FF6347"}, {"turquoise", "#40E0D0"},
            {"violet", "#EE82EE"}, {"wheat", "#F5DEB3"},
            {"white", "#FFFFFF"}, {"whitesmoke", "#F5F5F5"},
            {"yellow", "#FFFF00"}, {"yellowgreen", "#9ACD32"},
        };

        auto hex = name;
        if(name.empty()) return false;

        if(name[0] != '#')
        {
            auto lower = name;
            for(auto& c : lower) c = std::tolower(static_cast<unsigned char>(c));
            auto it = names.find(lower);
            if(it == names.end()) return false;
            hex = it->second;
        }

        // Parse hex: #RGB, #RRGGBB, #RRGGBBAA
        auto hex_digit = [](char c) -> int
        {
            if(c >= '0' && c <= '9') return c - '0';
            if(c >= 'a' && c <= 'f') return c - 'a' + 10;
            if(c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };

        if(hex.size() == 4) // #RGB
        {
            auto ri = hex_digit(hex[1]);
            auto gi = hex_digit(hex[2]);
            auto bi = hex_digit(hex[3]);
            if(ri < 0 || gi < 0 || bi < 0) return false;
            r = to_float(ri * 17); g = to_float(gi * 17); b = to_float(bi * 17); a = 1.0F;
            return true;
        }
        if(hex.size() == 7) // #RRGGBB
        {
            auto ri = hex_digit(hex[1]) * 16 + hex_digit(hex[2]);
            auto gi = hex_digit(hex[3]) * 16 + hex_digit(hex[4]);
            auto bi = hex_digit(hex[5]) * 16 + hex_digit(hex[6]);
            if(ri < 0 || gi < 0 || bi < 0) return false;
            r = to_float(ri); g = to_float(gi); b = to_float(bi); a = 1.0F;
            return true;
        }
        if(hex.size() == 9) // #RRGGBBAA
        {
            auto ri = hex_digit(hex[1]) * 16 + hex_digit(hex[2]);
            auto gi = hex_digit(hex[3]) * 16 + hex_digit(hex[4]);
            auto bi = hex_digit(hex[5]) * 16 + hex_digit(hex[6]);
            auto ai = hex_digit(hex[7]) * 16 + hex_digit(hex[8]);
            if(ri < 0 || gi < 0 || bi < 0 || ai < 0) return false;
            r = to_float(ri); g = to_float(gi); b = to_float(bi); a = to_float(ai);
            return true;
        }
        return false;
    }

    template<typename T>
    static void read_into(const ini_config& ini, const std::string& key, T& value)
    {
        if(ini.exists(key)) value = ini.get<T>(key);
    }

    static void read_into(const ini_config& ini, const std::string& key,
                               float& r, float& g, float& b, float& a)
    {
        if(!ini.exists(key)) return;
        auto value = ini.get<std::string>(key);
        if(!parse_css_color(value, r, g, b, a))
        {
            throw std::runtime_error("Unrecognized color: " + value);
        }
    }

    static feature_style load_style(const ini_config& ini, const std::string& prefix,
                                     const feature_style& fallback)
    {
        auto s = fallback;
        read_into(ini, prefix + "min_zoom", s.min_zoom);
        read_into(ini, prefix + "max_zoom", s.max_zoom);
        read_into(ini, prefix + "color", s.r, s.g, s.b, s.a);
        read_into(ini, prefix + "r", s.r);
        read_into(ini, prefix + "g", s.g);
        read_into(ini, prefix + "b", s.b);
        read_into(ini, prefix + "a", s.a);
        read_into(ini, prefix + "line_width", s.line_width);
        read_into(ini, prefix + "border_width", s.border_width);
        read_into(ini, prefix + "dash_length", s.dash_length);
        read_into(ini, prefix + "gap_length", s.gap_length);
        return s;
    }

    // All known feature keys that the renderer may look up
    static constexpr std::array all_keys = {
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
        "tfr",
        "sua_prohibited", "sua_restricted", "sua_warning",
        "sua_alert", "sua_moa", "sua_nsa",
        "runway",
        "rco", "awos",
        "route",
    };

    chart_style::chart_style(const std::string& ini_path, chart_mode mode)
    {
        std::ifstream test(ini_path);
        if(!test.good())
        {
            throw std::runtime_error("Chart style INI not found: " + ini_path);
        }
        test.close();

        ini_config ini(ini_path);
        auto suffix = mode_suffix(mode);

        feature_style defaults;
        for(const char* key : all_keys)
        {
            auto prefix = std::string(key) + "." + suffix + ".";
            styles[key] = load_style(ini, prefix, defaults);
        }
    }

    const feature_style& chart_style::get(const std::string& key) const
    {
        auto it = styles.find(key);
        if(it != styles.end())
        {
            return it->second;
        }
        static const feature_style fallback{0.0, 99.0, 1.0F, 0.0F, 1.0F, 1.0F, 20.0F, 5.0F, 30.0F, 15.0F};
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

    // TFR
    bool chart_style::tfr_visible(double zoom) const
    { return visible("tfr", zoom); }
    const feature_style& chart_style::tfr_style() const
    { return get("tfr"); }

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

    // RCO
    bool chart_style::rco_visible(double zoom) const
    { return visible("rco", zoom); }
    const feature_style& chart_style::rco_style() const
    { return get("rco"); }

    // AWOS
    bool chart_style::awos_visible(double zoom) const
    { return visible("awos", zoom); }
    const feature_style& chart_style::awos_style() const
    { return get("awos"); }

    // Route
    const feature_style& chart_style::route_style() const
    { return get("route"); }

    // Group-level early-out helpers
    bool chart_style::any_airport_visible(double zoom) const
    {
        static constexpr std::array keys = {
            "airport_class_b", "airport_class_c", "airport_class_d",
            "airport_class_e", "airport_other"};
        return std::any_of(keys.begin(), keys.end(),
            [&](const char* k) { return visible(k, zoom); });
    }

    bool chart_style::any_navaid_visible(double zoom) const
    {
        return visible("navaid_vor", zoom) || visible("navaid_ndb", zoom);
    }

    bool chart_style::any_fix_visible(double zoom) const
    {
        return visible("fix_airway", zoom) || visible("fix_noairway", zoom);
    }

    bool chart_style::any_obstacle_visible(double zoom) const
    {
        return visible("obstacle_1000ft", zoom)
            || visible("obstacle_200ft", zoom)
            || visible("obstacle_low", zoom);
    }

    bool chart_style::any_airway_visible(double zoom) const
    {
        static constexpr std::array keys = {
            "airway_v", "airway_t", "airway_br", "airway_tk",
            "airway_j", "airway_q", "airway_ar", "airway_rte", "airway_h",
            "airway_m", "airway_l", "airway_y", "airway_w", "airway_n",
            "airway_r", "airway_a", "airway_g", "airway_b"};
        return std::any_of(keys.begin(), keys.end(),
            [&](const char* k) { return visible(k, zoom); });
    }

    bool chart_style::any_airspace_visible(double zoom) const
    {
        static constexpr std::array keys = {
            "airspace_b", "airspace_c", "airspace_d",
            "airspace_e2", "airspace_e3", "airspace_e4",
            "airspace_e5", "airspace_e6", "airspace_e7"};
        return std::any_of(keys.begin(), keys.end(),
            [&](const char* k) { return visible(k, zoom); });
    }

    bool chart_style::any_sua_visible(double zoom) const
    {
        static constexpr std::array keys = {
            "sua_prohibited", "sua_restricted", "sua_warning",
            "sua_alert", "sua_moa", "sua_nsa"};
        return std::any_of(keys.begin(), keys.end(),
            [&](const char* k) { return visible(k, zoom); });
    }

    bool chart_style::any_artcc_visible(double zoom) const
    {
        return visible("artcc_low", zoom)
            || visible("artcc_high", zoom)
            || visible("artcc_oceanic", zoom);
    }

    // SQL-filter helpers
    chart_style::filter_list chart_style::visible_airport_classes(double zoom) const
    {
        // "airport_other" catches everything not in {B,C,D,E}; if it's visible,
        // skip SQL filtering (pass all rows through).
        if(visible("airport_other", zoom)) return std::nullopt;
        std::vector<std::string> out;
        if(visible("airport_class_b", zoom)) out.emplace_back("B");
        if(visible("airport_class_c", zoom)) out.emplace_back("C");
        if(visible("airport_class_d", zoom)) out.emplace_back("D");
        if(visible("airport_class_e", zoom)) out.emplace_back("E");
        return out;
    }

    chart_style::filter_list chart_style::visible_airspace_values(double zoom) const
    {
        // No "other" bucket — every airspace row fits a specific key. Collect
        // both CLASS values (B/C/D) and LOCAL_TYPE values (CLASS_E2..E7).
        // SQL filter is "CLASS IN (...) OR LOCAL_TYPE IN (...)" against this
        // same list (the two columns' value spaces don't overlap).
        std::vector<std::string> out;
        if(visible("airspace_b", zoom)) out.emplace_back("B");
        if(visible("airspace_c", zoom)) out.emplace_back("C");
        if(visible("airspace_d", zoom)) out.emplace_back("D");
        if(visible("airspace_e2", zoom)) out.emplace_back("CLASS_E2");
        if(visible("airspace_e3", zoom)) out.emplace_back("CLASS_E3");
        if(visible("airspace_e4", zoom)) out.emplace_back("CLASS_E4");
        if(visible("airspace_e5", zoom)) out.emplace_back("CLASS_E5");
        if(visible("airspace_e6", zoom)) out.emplace_back("CLASS_E6");
        if(visible("airspace_e7", zoom)) out.emplace_back("CLASS_E7");
        return out;
    }

    chart_style::filter_list chart_style::visible_sua_types(double zoom) const
    {
        // "sua_moa" catches MOA plus anything unrecognized; if visible, skip filter.
        if(visible("sua_moa", zoom)) return std::nullopt;
        std::vector<std::string> out;
        if(visible("sua_restricted", zoom)) out.emplace_back("RA");
        if(visible("sua_prohibited", zoom)) out.emplace_back("PA");
        if(visible("sua_warning", zoom)) out.emplace_back("WA");
        if(visible("sua_alert", zoom)) out.emplace_back("AA");
        if(visible("sua_nsa", zoom)) out.emplace_back("NSA");
        return out;
    }

} // namespace nasrbrowse
