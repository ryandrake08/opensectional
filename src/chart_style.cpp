#include "chart_style.hpp"
#include "ini_config.hpp"
#include "nasr_database.hpp"
#include <algorithm>
#include <array>
#include <stdexcept>
#include <unordered_map>

namespace osect
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

    static void apply_overrides(feature_style& s, const ini_config& ini,
                                 const std::string& prefix)
    {
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
    }

    // Per-key VFR defaults. Mirrors osect.ini row-for-row. Color is a CSS
    // name (or hex) parsed at construction time; an empty color string
    // leaves r/g/b/a at feature_style{}'s white default — used by zoom-only
    // keys like airport_class_*, fix_airway/noairway whose color comes from
    // a sibling key.
    struct style_default
    {
        const char* key;
        double min_zoom;
        const char* color;          // "" → white default
        float line_width;
        float dash_length;
        float gap_length;
    };

    static constexpr std::array<style_default, 70> vfr_defaults = {{
        // Airports — zoom thresholds keyed by airspace class
        {"airport_class_b",   7.0,  "",                1.0F, 0.0F, 0.0F},
        {"airport_class_c",   8.0,  "",                1.0F, 0.0F, 0.0F},
        {"airport_class_d",   9.0,  "",                1.0F, 0.0F, 0.0F},
        {"airport_class_e",  10.0,  "",                1.0F, 0.0F, 0.0F},
        {"airport_other",    11.0,  "",                1.0F, 0.0F, 0.0F},
        // Airports — colors by tower presence
        {"airport_towered",   0.0,  "royalblue",       1.0F, 0.0F, 0.0F},
        {"airport_untowered", 0.0,  "mediumorchid",    1.0F, 0.0F, 0.0F},
        // Navaids
        {"navaid_vor",        9.0,  "limegreen",       1.0F, 0.0F, 0.0F},
        {"navaid_ndb",        9.0,  "saddlebrown",     1.0F, 0.0F, 0.0F},
        // Fixes — zoom thresholds by airway membership
        {"fix_airway",        9.0,  "",                1.0F, 0.0F, 0.0F},
        {"fix_noairway",     10.0,  "",                1.0F, 0.0F, 0.0F},
        // Fixes — colors by use_code
        {"fix_wp",            0.0,  "limegreen",       1.0F, 0.0F, 0.0F},
        {"fix_rp",            0.0,  "limegreen",       1.0F, 0.0F, 0.0F},
        {"fix_vfr",           0.0,  "saddlebrown",     1.0F, 0.0F, 0.0F},
        {"fix_cn",            0.0,  "forestgreen",     1.0F, 0.0F, 0.0F},
        {"fix_mr",            0.0,  "gray",            1.0F, 0.0F, 0.0F},
        {"fix_mw",            0.0,  "gray",            1.0F, 0.0F, 0.0F},
        {"fix_nrs",           0.0,  "darkslategray",   1.0F, 0.0F, 0.0F},
        // RCO / AWOS
        {"rco",              11.0,  "teal",            1.0F, 0.0F, 0.0F},
        {"awos",             11.0,  "teal",            1.0F, 0.0F, 0.0F},
        // Obstacles by AGL height
        {"obstacle_1000ft",  12.0,  "crimson",         1.0F, 0.0F, 0.0F},
        {"obstacle_200ft",   13.0,  "orange",          1.0F, 0.0F, 0.0F},
        {"obstacle_low",     14.0,  "darkgray",        1.0F, 0.0F, 0.0F},
        // Airways — low-altitude conventional/RNAV (dark cyan on VFR)
        {"airway_v",          9.0,  "darkcyan",        1.0F, 0.0F, 0.0F},
        {"airway_t",          9.0,  "darkcyan",        1.0F, 0.0F, 0.0F},
        {"airway_tk",         9.0,  "darkcyan",        1.0F, 0.0F, 0.0F},
        // Airways — high-altitude (IFR-high colors, gated by High band)
        {"airway_j",          9.0,  "black",           1.0F, 0.0F, 0.0F},
        {"airway_q",          9.0,  "darkblue",        1.0F, 0.0F, 0.0F},
        {"airway_y",          9.0,  "darkblue",        1.0F, 0.0F, 0.0F},
        {"airway_h",          9.0,  "black",           1.0F, 0.0F, 0.0F},
        // Airways — dual-band (ICAO ATS / Bahama / Atlantic / PR)
        {"airway_m",          9.0,  "darkcyan",        1.0F, 0.0F, 0.0F},
        {"airway_l",          9.0,  "darkcyan",        1.0F, 0.0F, 0.0F},
        {"airway_w",          9.0,  "darkcyan",        1.0F, 0.0F, 0.0F},
        {"airway_n",          9.0,  "darkcyan",        1.0F, 0.0F, 0.0F},
        {"airway_br",         9.0,  "darkcyan",        1.0F, 0.0F, 0.0F},
        {"airway_ar",         9.0,  "darkcyan",        1.0F, 0.0F, 0.0F},
        {"airway_rte",        9.0,  "darkcyan",        1.0F, 0.0F, 0.0F},
        // Airways — LF/MF colored (low-altitude, magenta on VFR)
        {"airway_r",          9.0,  "darkmagenta",     1.0F, 0.0F, 0.0F},
        {"airway_a",          9.0,  "darkmagenta",     1.0F, 0.0F, 0.0F},
        {"airway_g",          9.0,  "darkmagenta",     1.0F, 0.0F, 0.0F},
        {"airway_b",          9.0,  "darkmagenta",     1.0F, 0.0F, 0.0F},
        // MTR
        {"mtr",               9.0,  "gray",            1.0F, 0.0F, 0.0F},
        // PJA
        {"pja_area",          8.0,  "tan",             1.0F, 12.0F, 6.0F},
        {"pja_point",         8.0,  "tan",             1.0F, 0.0F, 0.0F},
        // MAA
        {"maa_area",          8.0,  "tan",             1.0F, 12.0F, 6.0F},
        {"maa_point",         8.0,  "tan",             1.0F, 0.0F, 0.0F},
        // ADIZ
        {"adiz",              6.0,  "orchid",          2.0F, 0.0F, 0.0F},
        // Class airspace
        {"airspace_b",        5.0,  "royalblue",       2.0F, 0.0F, 0.0F},
        {"airspace_c",        6.0,  "darkorchid",      2.0F, 0.0F, 0.0F},
        {"airspace_d",        6.0,  "royalblue",       1.0F, 12.0F, 6.0F},
        {"airspace_e2",       9.0,  "darkorchid",      1.0F, 12.0F, 6.0F},
        {"airspace_e3",       9.0,  "darkorchid",      1.0F, 12.0F, 6.0F},
        {"airspace_e4",       9.0,  "darkorchid",      1.0F, 12.0F, 6.0F},
        // E5/E6/E7 are not depicted on VFR — min_zoom=99 hides them
        {"airspace_e5",      99.0,  "",                1.0F, 0.0F, 0.0F},
        {"airspace_e6",      99.0,  "",                1.0F, 0.0F, 0.0F},
        {"airspace_e7",      99.0,  "",                1.0F, 0.0F, 0.0F},
        // ARTCC
        {"artcc_low",         3.0,  "midnightblue",    1.0F, 0.0F, 0.0F},
        {"artcc_high",        3.0,  "midnightblue",    1.0F, 0.0F, 0.0F},
        {"artcc_unlimited_fir",  3.0,  "midnightblue", 1.0F, 0.0F, 0.0F},
        {"artcc_unlimited_cta", 99.0, "",              1.0F, 0.0F, 0.0F},
        {"artcc_unlimited_uta", 99.0, "",              1.0F, 0.0F, 0.0F},
        // SUA
        {"sua_prohibited",    6.0,  "crimson",         2.0F, 0.0F, 0.0F},
        {"sua_restricted",    6.0,  "orange",          2.0F, 0.0F, 0.0F},
        {"sua_warning",       6.0,  "orange",          2.0F, 15.0F, 8.0F},
        {"sua_alert",         9.0,  "plum",            2.0F, 0.0F, 0.0F},
        {"sua_moa",           7.0,  "plum",            2.0F, 15.0F, 8.0F},
        {"sua_nsa",           9.0,  "saddlebrown",     2.0F, 0.0F, 0.0F},
        // Runway
        {"runway",           10.0,  "silver",          3.0F, 0.0F, 0.0F},
        // Route
        {"route",             0.0,  "magenta",         3.0F, 0.0F, 0.0F},
        // TFR
        {"tfr",               6.0,  "crimson",         2.0F, 15.0F, 8.0F},
    }};

    static feature_style resolve_default(const style_default& d)
    {
        feature_style s;
        s.min_zoom = d.min_zoom;
        s.line_width = d.line_width;
        s.dash_length = d.dash_length;
        s.gap_length = d.gap_length;
        // Match osect.ini's runway override: explicit border_width=0
        if(std::string(d.key) == "runway") s.border_width = 0.0F;
        // Route ships with border_width=1, same as the struct default;
        // no special-case needed.
        if(d.color && d.color[0] != '\0')
        {
            if(!parse_css_color(d.color, s.r, s.g, s.b, s.a))
            {
                throw std::runtime_error(
                    std::string("chart_style: bad default color '") + d.color +
                    "' for key '" + d.key + "'");
            }
        }
        return s;
    }

    chart_style::chart_style(const ini_config& ini, chart_mode mode)
    {
        auto suffix = mode_suffix(mode);

        for(const auto& d : vfr_defaults)
        {
            auto s = resolve_default(d);
            apply_overrides(s, ini, std::string(d.key) + "." + suffix + ".");
            styles[d.key] = s;
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

    // Walk the style keys applicable to an ARTCC polygon. Domestic sectors
    // map by ALTITUDE (LOW/HIGH). Oceanic (UNLIMITED) polygons map by TYPE,
    // and a CTA/FIR polygon is drawn under both the CTA and FIR styles.
    template <typename F>
    static void for_each_artcc_key(const std::string& altitude,
                                    const std::string& type, const F& f)
    {
        if(altitude == "LOW") { f("artcc_low"); return; }
        if(altitude == "HIGH") { f("artcc_high"); return; }
        if(type == "UTA") { f("artcc_unlimited_uta"); return; }
        if(type == "CTA") { f("artcc_unlimited_cta"); return; }
        if(type == "FIR") { f("artcc_unlimited_fir"); return; }
        if(type == "CTA/FIR")
        {
            f("artcc_unlimited_cta");
            f("artcc_unlimited_fir");
            return;
        }
        f("artcc_low");
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

    // ARTCC — a single polygon may map to multiple style keys (CTA/FIR → both
    // CTA and FIR). artcc_visible returns true if any mapped key is visible;
    // artcc_style returns the primary (first) mapped style, used for selection
    // highlight and labels. Rendering of all applicable styles is via
    // for_each_visible_artcc_style on chart_style.
    bool chart_style::artcc_visible(const std::string& altitude,
                                     const std::string& type, double zoom) const
    {
        bool any_visible = false;
        for_each_artcc_key(altitude, type, [&](const char* key) {
            if(visible(key, zoom)) any_visible = true;
        });
        return any_visible;
    }
    const feature_style& chart_style::artcc_style(const std::string& altitude,
                                                    const std::string& type) const
    {
        const feature_style* first = nullptr;
        for_each_artcc_key(altitude, type, [&](const char* key) {
            if(!first) first = &get(key);
        });
        return *first;
    }
    void chart_style::for_each_visible_artcc_style(
        const std::string& altitude, const std::string& type, double zoom,
        const std::function<void(const feature_style&)>& f) const
    {
        for_each_artcc_key(altitude, type, [&](const char* key) {
            if(visible(key, zoom)) f(get(key));
        });
    }

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
            || visible("artcc_unlimited_cta", zoom)
            || visible("artcc_unlimited_fir", zoom)
            || visible("artcc_unlimited_uta", zoom);
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

} // namespace osect
