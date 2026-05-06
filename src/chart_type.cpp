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

    bool airport_on_chart(const airport& a, chart_type ct)
    {
        // Closed / non-operational airports are not drawn on any chart.
        if(a.arpt_status != "O")
        {
            return false;
        }
        switch(ct)
        {
        case chart_type::sectional:
            // VFR Sectional depicts every operational airport regardless
            // of size, surface, or use.
            return true;
        case chart_type::ifr_low:
            // IFR Enroute Low depicts airports with a published IAP, plus
            // any hard-surface field with runway ≥ 3000 ft (alternate
            // capable). The IAP indicator is a NASR-only proxy — see
            // build_nasr.py: HAS_IAP_INDICATOR.
            if(a.has_iap_indicator)
            {
                return true;
            }
            return a.hard_surface && a.max_hard_rwy_len >= 3000;
        case chart_type::ifr_high:
            // IFR Enroute High depicts hard-surface fields with
            // runway ≥ 5000 ft (alternate-eligible at jet altitudes).
            return a.hard_surface && a.max_hard_rwy_len >= 5000;
        }
        return false;
    }

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
            // Standalone TACAN is military-only and not depicted on
            // civilian VFR Sectionals. Verified against NQX (Key West),
            // NKX (Miramar), CSG (Columbus) — none appear on the
            // sectional. VORTAC (a VOR collocated with TACAN) is a
            // separate NAV_TYPE and remains on Sectional.
            return n.nav_type != "TACAN";
        case chart_type::ifr_low:
            // Every operational navaid is in the low-altitude structure:
            // even ALT_CODE='H' VORs (e.g. AKN, DLG, ODK) serve
            // low-altitude users within their lower service-volume radius
            // and are charted on IFR Low. TACANs are also depicted on
            // IFR Low.
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
        {
            // Explicit VFR/sectional chart tags.
            if(charts_has_token(f.charts, "SECTIONAL") || charts_has_token(f.charts, "VFR FLYWAY PLANNING") ||
               charts_has_token(f.charts, "VFR TERMINAL AREA"))
            {
                return true;
            }
            // Beyond the explicit tags, sectional inclusion piggybacks on
            // en-route-structure membership — a fix appearing only on
            // terminal procedures (IAP/SID/STAR plates without an
            // en-route role) is not depicted on Sectionals.
            if(!charts_has_token(f.charts, "ENROUTE LOW"))
            {
                return false;
            }
            // RNAV waypoints (FIX_USE_CODE='WP') on the en-route
            // structure are navigation aids and are depicted on
            // Sectionals (e.g. SMONE: WP + IAP + ENROUTE LOW).
            if(f.use_code == "WP")
            {
                return true;
            }
            // STAR / SID transition fixes on the en-route structure are
            // depicted on Sectionals (e.g. KARNN: RP + STAR + ENROUTE LOW;
            // HENCE: RP + SID + ENROUTE LOW).
            if(charts_has_token(f.charts, "STAR") || charts_has_token(f.charts, "SID"))
            {
                return true;
            }
            // Reporting points in TRACON jurisdiction (`CONTROLLER`
            // un-suffixed = terminal-area approach control, distinct from
            // `CONTROLLER LOW`/`CONTROLLER HIGH` = ARTCC sectors) on the
            // en-route structure are depicted on Sectionals unless they're
            // also IAP intermediate/final fixes — in which case they
            // belong on the IAP plate, not the Sectional. WINDY
            // (CONTROLLER+ENROUTE LOW, no IAP) shows; HONEZ
            // (CONTROLLER+ENROUTE LOW+IAP) doesn't.
            if(f.use_code == "RP" && charts_has_token(f.charts, "CONTROLLER") && !charts_has_token(f.charts, "IAP"))
            {
                return true;
            }
            return false;
        }
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
