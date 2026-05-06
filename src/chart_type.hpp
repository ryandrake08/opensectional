#pragma once

#include <string>

namespace osect
{
    // Which published FAA chart product the user wants to render. Drives
    // visibility for features whose chart presence is editorially decided
    // (navaids, fixes, airways, obstacles, MTRs). Independent of the
    // altitude_filter, which drives features with genuine altitude
    // semantics (SUA, ARTCC).
    enum class chart_type
    {
        sectional, // VFR Sectional
        ifr_low,   // IFR Enroute Low
        ifr_high,  // IFR Enroute High
    };

    struct navaid;
    struct fix;

    // Per-feature chart-membership rules. Each predicate returns true iff
    // the feature would be drawn on the corresponding chart product.
    bool navaid_on_chart(const navaid& n, chart_type ct);
    bool fix_on_chart(const fix& f, chart_type ct);
    bool airway_on_chart(const std::string& awy_id, chart_type ct);
    bool obstacle_on_chart(chart_type ct);
    bool mtr_on_chart(const std::string& route_type_code, chart_type ct);

} // namespace osect
