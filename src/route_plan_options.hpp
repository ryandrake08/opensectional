#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace nasrbrowse
{
    // Cost-modifier presets used by route_plan_options. Values
    // match g3xfplan: PREFER discounts, INCLUDE is neutral, AVOID
    // penalizes mildly, REJECT effectively excludes.
    inline constexpr double cost_prefer  = 0.8;
    inline constexpr double cost_include = 1.0;
    inline constexpr double cost_avoid   = 1.25;
    inline constexpr double cost_reject  = 1000.0;

    // Fine-grained per-node classification. Drives the
    // per-waypoint cost modifier in the A* search. Layout matters:
    // `count` is used to size the modifier array.
    enum class wp_subtype : std::uint8_t
    {
        // Airports (SITE_TYPE_CODE)
        airport_landplane,    // A
        airport_balloonport,  // B
        airport_seaplane,     // C
        airport_gliderport,   // G
        airport_heliport,     // H
        airport_ultralight,   // U

        // Navaids (NAV_TYPE)
        navaid_vor,
        navaid_vortac,
        navaid_vor_dme,
        navaid_dme,
        navaid_ndb,
        navaid_ndb_dme,

        // Fixes (FIX_USE_CODE)
        fix_wp,    // generic waypoint
        fix_rp,    // reporting point
        fix_cn,    // coordination fix
        fix_mr,    // military reporting
        fix_vfr,   // VFR waypoint

        count,
        unknown = count
    };

    // Coarse classification of an airway, derived from the first
    // one or two characters of the airway ID.
    enum class awy_class : std::uint8_t
    {
        victor,   // V*
        jet,      // J*
        rnav,     // T*, Q*, TK*
        color,    // G*, A*, R*, B* (excluding AT/AR/BR/BF prefixes)
        other,    // AT, AR, BR, BF, PA, PR, anything else

        count
    };

    // Configuration for the A* route planner. Held by callers (the
    // ini-driven defaults populate one of these; the GUI overlays
    // its knobs onto it; route_submitter passes the result to the
    // planner per submission).
    struct route_plan_options
    {
        // Maximum direct-leg length, in nautical miles. A direct
        // step longer than this is multiplied by `cost_reject`.
        double max_leg_length_nm = 80.0;

        // Per-subtype cost modifier. Default-initialized to
        // `cost_include` so a default-constructed value is
        // uniform-cost.
        std::array<double, static_cast<std::size_t>(wp_subtype::count)>
            wp_cost;

        // Per-airway-class cost modifier. Only applied when
        // `use_airways` is true.
        std::array<double, static_cast<std::size_t>(awy_class::count)>
            awy_cost;

        // Cost factor for an airway edge marked `is_gap`. Applied
        // in place of `awy_cost[type]` for that edge, and only
        // when `use_airways` is true. Default `cost_include` (1.0)
        // treats the bridge across a gap as cost-neutral relative
        // to a generic direct hop.
        double gap_cost = cost_include;

        // When true: airway-routable navaid and fix subtypes
        // (VOR/VORTAC/VOR-DME/DME/NDB/NDB-DME and the WP/RP/CN/MR
        // fixes) are forced to `cost_include` regardless of their
        // configured `wp_cost`, and `awy_cost` modifies on-airway
        // edges. When false: `wp_cost` is honored verbatim and
        // airway edges receive no special treatment.
        bool use_airways = false;

        route_plan_options()
            : wp_cost{}
            , awy_cost{}
        {
            wp_cost.fill(cost_include);
            awy_cost.fill(cost_include);
        }
    };

} // namespace nasrbrowse
