#pragma once

#include <string>

namespace nasrbrowse
{

// Selects which altitude bands the user wants rendered. The 18,000 ft MSL
// divider is the standard boundary between low-altitude and high-altitude
// aeronautical depictions (VFR / IFR-low are low-band; IFR-high is high-band).
//
// A feature is shown iff at least one enabled band overlaps the feature's
// altitude relevance. If neither band is enabled, nothing renders.
struct altitude_filter
{
    bool show_low = true;   // features relevant below 18,000 ft MSL
    bool show_high = false; // features relevant at or above 18,000 ft MSL

    static constexpr int DIVIDER_FT = 18000;

    // Sentinel written by build_nasr_db for UNL/unlimited upper bounds.
    static constexpr int UNLIMITED_FT = 99999;

    bool any() const { return show_low || show_high; }

    // True iff a feature occupying [lower_ft, upper_ft] MSL should be shown.
    bool overlaps(int lower_ft, int upper_ft) const
    {
        if (!any()) return false;
        bool reaches_low = lower_ft < DIVIDER_FT;
        bool reaches_high = upper_ft >= DIVIDER_FT;
        return (show_low && reaches_low) || (show_high && reaches_high);
    }

    // Convenience for features classified by sub-type rather than range.
    bool low_enabled() const { return show_low; }
    bool high_enabled() const { return show_high; }

    bool operator==(const altitude_filter& o) const
    {
        return show_low == o.show_low && show_high == o.show_high;
    }
    bool operator!=(const altitude_filter& o) const { return !(*this == o); }
};

// Airway prefix → altitude band bitmask (bit 0 = low, bit 1 = high).
// Derived from a scan of AWY_SEG_ALT source data; see
// .ignore/airway_colors.txt.
inline unsigned airway_bands(const std::string& awy_id)
{
    if(awy_id.empty()) return 0b11;
    if(awy_id.size() >= 2)
    {
        const char* p = awy_id.c_str();
        if(p[0] == 'T' && p[1] == 'K') return 0b01;
        if(p[0] == 'B' && p[1] == 'R') return 0b11;
        if(p[0] == 'A' && p[1] == 'R') return 0b11;
        if(awy_id.compare(0, 3, "RTE") == 0) return 0b11;
    }
    switch(awy_id[0])
    {
        case 'V': case 'T':                          return 0b01;
        case 'J': case 'Q': case 'Y': case 'H':      return 0b10;
        case 'M': case 'L': case 'W': case 'N':      return 0b11;
        case 'R': case 'A': case 'G': case 'B':      return 0b01;
        default:                                     return 0b11;
    }
}

// ARTCC altitude string → altitude band bitmask. LOW → low, HIGH/OCEANIC → high.
inline unsigned artcc_bands(const std::string& altitude)
{
    return altitude == "LOW" ? 0b01 : 0b10;
}

// MTR route type → altitude band bitmask. VR (VFR military) is low-only;
// IR (IFR military) spans both bands since CSV lacks per-segment altitudes.
inline unsigned mtr_bands(const std::string& route_type_code)
{
    return route_type_code == "VR" ? 0b01 : 0b11;
}

inline bool altitude_filter_allows(const altitude_filter& af, unsigned bands)
{
    return ((bands & 0b01) && af.show_low) || ((bands & 0b10) && af.show_high);
}

} // namespace nasrbrowse
