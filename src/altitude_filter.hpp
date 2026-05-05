#pragma once

#include <string>

namespace osect
{

    // Selects which altitude band the user wants rendered. The 18,000 ft MSL
    // divider is the standard boundary between low-altitude and high-altitude
    // aeronautical depictions (VFR / IFR-low are low-band; IFR-high is high-band).
    // The "unlimited" band covers oceanic ARTCC surfaces (CTA, FIR, CTA/FIR, UTA)
    // which are not split at 18,000 ft.
    //
    // UI enforces mutually-exclusive selection (exactly one band active).
    struct altitude_filter
    {
        bool show_low = true;        // features relevant below 18,000 ft MSL
        bool show_high = false;      // features relevant at or above 18,000 ft MSL
        bool show_unlimited = false; // oceanic / unlimited stratum

        static constexpr int DIVIDER_FT = 18000;

        // Sentinel written by build_nasr_db for UNL/unlimited upper bounds.
        static constexpr int UNLIMITED_FT = 99999;

        bool any() const
        {
            return show_low || show_high || show_unlimited;
        }

        // True iff a feature occupying [lower_ft, upper_ft] MSL should be shown.
        bool overlaps(int lower_ft, int upper_ft) const
        {
            if(!any())
            {
                return false;
            }
            bool reaches_low = lower_ft < DIVIDER_FT;
            bool reaches_high = upper_ft >= DIVIDER_FT;
            return (show_low && reaches_low) || (show_high && reaches_high);
        }

        // Reference-aware variant for SUA strata. Each bound is (value,
        // ref) where ref ∈ {"MSL","SFC","STD","OTHER"}. We classify each
        // bound's reach into low/high without a DEM:
        //   MSL  — direct compare against the 18,000 ft divider.
        //   STD  — same (FL180 ≈ 18,000 by chart convention).
        //   SFC  — AGL value: in practice always low-band-only (max US
        //          terrain plus typical SFC ceilings stays below FL180).
        //   OTHER (UNL) — unbounded; treat as both low and high.
        // The lower bound determines whether the feature "reaches low";
        // the upper bound determines whether it "reaches high".
        bool overlaps(int lower_ft, const std::string& lower_ref, int upper_ft, const std::string& upper_ref) const
        {
            if(!any())
            {
                return false;
            }
            bool reaches_low;
            if(lower_ref == "OTHER")
            {
                reaches_low = true;
            }
            else if(lower_ref == "SFC")
            {
                reaches_low = true; // AGL → always low
            }
            else /* MSL or STD */
            {
                reaches_low = lower_ft < DIVIDER_FT;
            }

            bool reaches_high;
            if(upper_ref == "OTHER")
            {
                reaches_high = true;
            }
            else if(upper_ref == "SFC")
            {
                reaches_high = false; // AGL ceilings rarely cross FL180
            }
            else /* MSL or STD */
            {
                reaches_high = upper_ft >= DIVIDER_FT;
            }

            return (show_low && reaches_low) || (show_high && reaches_high);
        }

        // Convenience for features classified by sub-type rather than range.
        bool low_enabled() const
        {
            return show_low;
        }
        bool high_enabled() const
        {
            return show_high;
        }
        bool unlimited_enabled() const
        {
            return show_unlimited;
        }

        bool operator==(const altitude_filter& o) const
        {
            return show_low == o.show_low && show_high == o.show_high && show_unlimited == o.show_unlimited;
        }
        bool operator!=(const altitude_filter& o) const
        {
            return !(*this == o);
        }
    };

    // Airway prefix → altitude band bitmask (bit 0 = low, bit 1 = high).
    // Derived from a scan of AWY_SEG_ALT source data; see
    // .ignore/airway_colors.txt.
    inline unsigned airway_bands(const std::string& awy_id)
    {
        if(awy_id.empty())
        {
            return 0b11;
        }
        if(awy_id.size() >= 2)
        {
            const char* p = awy_id.c_str();
            if(p[0] == 'T' && p[1] == 'K')
            {
                return 0b01;
            }
            if(p[0] == 'B' && p[1] == 'R')
            {
                return 0b11;
            }
            if(p[0] == 'A' && p[1] == 'R')
            {
                return 0b11;
            }
            if(awy_id.compare(0, 3, "RTE") == 0)
            {
                return 0b11;
            }
        }
        switch(awy_id[0])
        {
        case 'V':
        case 'T':
            return 0b01;
        case 'J':
        case 'Q':
        case 'Y':
        case 'H':
            return 0b10;
        case 'M':
        case 'L':
        case 'W':
        case 'N':
            return 0b11;
        case 'R':
        case 'A':
        case 'G':
        case 'B':
            return 0b01;
        default:
            return 0b11;
        }
    }

    // ARTCC altitude string → altitude band bitmask. LOW → low, HIGH → high,
    // UNLIMITED (oceanic CTA/FIR/UTA) → unlimited.
    inline unsigned artcc_bands(const std::string& altitude)
    {
        if(altitude == "LOW")
        {
            return 0b001;
        }
        if(altitude == "HIGH")
        {
            return 0b010;
        }
        return 0b100;
    }

    // MTR route type → altitude band bitmask. VR (VFR military) is low-only;
    // IR (IFR military) spans both bands since CSV lacks per-segment altitudes.
    inline unsigned mtr_bands(const std::string& route_type_code)
    {
        return route_type_code == "VR" ? 0b01 : 0b11;
    }

    inline bool altitude_filter_allows(const altitude_filter& af, unsigned bands)
    {
        return ((bands & 0b001) && af.show_low) || ((bands & 0b010) && af.show_high) ||
               ((bands & 0b100) && af.show_unlimited);
    }

} // namespace osect
