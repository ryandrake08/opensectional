#include "chart_type.hpp"
#include "nasr_database.hpp"

#include <string_view>

namespace osect
{
    namespace
    {
        // Match a single comma-separated token against `charts` exactly
        // (no substring matching — "ENROUTE LOW" must not match
        // "CONTROLLER LOW").
        bool charts_has_token(const std::string& charts, std::string_view token)
        {
            std::size_t pos = 0;
            while(pos <= charts.size())
            {
                auto end = charts.find(',', pos);
                if(end == std::string::npos)
                {
                    end = charts.size();
                }
                if(end - pos == token.size() && charts.compare(pos, token.size(), token.data(), token.size()) == 0)
                {
                    return true;
                }
                pos = end + 1;
            }
            return false;
        }
    } // namespace

    bool navaid_on_chart(const navaid& n, chart_type ct)
    {
        // Test-only / non-charted navaid types — never depicted on any
        // chart product. Keeps this rule the single point of truth so
        // callers don't need to pre-filter.
        if(n.nav_type == "VOT" || n.nav_type == "FAN MARKER" || n.nav_type == "MARINE NDB")
        {
            return false;
        }
        switch(ct)
        {
        case chart_type::sectional:
        case chart_type::ifr_low:
            // Every operational navaid is in the low-altitude / sectional
            // structure: even ALT_CODE='H' VORs (e.g. AKN, DLG, ODK)
            // serve low-altitude users within their lower service-volume
            // radius and are charted on both Sectional and IFR Low.
            return true;
        case chart_type::ifr_high:
            // High-structure inclusion: VOR-class with ALT_CODE 'H' or 'VH',
            // or any navaid explicitly promoted by LOW_NAV_ON_HIGH_CHART_FLAG.
            return n.alt_code == "H" || n.alt_code == "VH" || n.low_nav_on_high_chart_flag == "Y";
        }
        return false;
    }

    bool fix_on_chart(const fix& f, chart_type ct)
    {
        switch(ct)
        {
        case chart_type::sectional:
            return charts_has_token(f.charts, "SECTIONAL") || charts_has_token(f.charts, "VFR FLYWAY PLANNING") ||
                   charts_has_token(f.charts, "VFR TERMINAL AREA");
        case chart_type::ifr_low:
            return charts_has_token(f.charts, "ENROUTE LOW");
        case chart_type::ifr_high:
            return charts_has_token(f.charts, "ENROUTE HIGH");
        }
        return false;
    }

    bool airway_on_chart(const std::string& awy_id, chart_type ct)
    {
        // Sectional inclusion is restricted to V (Victor) and T (T-route GPS)
        // — verified visually. Other low-altitude prefixes (R, A, G, B, etc.)
        // remain IFR-Low only as in the prior airway_bands behavior.
        if(awy_id.empty())
        {
            return ct == chart_type::ifr_low || ct == chart_type::ifr_high;
        }
        const char c0 = awy_id[0];
        const char c1 = awy_id.size() >= 2 ? awy_id[1] : 0;

        if(c0 == 'T' && c1 == 'K')
        {
            return ct == chart_type::ifr_low;
        }
        if((c0 == 'B' && c1 == 'R') || (c0 == 'A' && c1 == 'R'))
        {
            return ct == chart_type::ifr_low || ct == chart_type::ifr_high;
        }
        if(awy_id.compare(0, 3, "RTE") == 0)
        {
            return ct == chart_type::ifr_low || ct == chart_type::ifr_high;
        }

        switch(c0)
        {
        case 'V':
        case 'T':
            return ct == chart_type::sectional || ct == chart_type::ifr_low;
        case 'J':
        case 'Q':
        case 'Y':
        case 'H':
            return ct == chart_type::ifr_high;
        case 'M':
        case 'L':
        case 'W':
        case 'N':
            return ct == chart_type::ifr_low || ct == chart_type::ifr_high;
        case 'R':
        case 'A':
        case 'G':
        case 'B':
            return ct == chart_type::ifr_low;
        default:
            return ct == chart_type::ifr_low || ct == chart_type::ifr_high;
        }
    }

    bool obstacle_on_chart(chart_type ct)
    {
        return ct == chart_type::sectional;
    }

    bool mtr_on_chart(const std::string& route_type_code, chart_type ct)
    {
        if(route_type_code == "VR")
        {
            return ct == chart_type::sectional;
        }
        return ct == chart_type::sectional || ct == chart_type::ifr_low;
    }

} // namespace osect
