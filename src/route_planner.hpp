#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace nasrbrowse
{
    class nasr_database;

    // A* pathfinder over the NASR waypoint graph. Constructed once
    // from the database; holds an immutable in-memory catalog of
    // routable waypoints plus an airway adjacency map.
    class route_planner
    {
        struct impl;
        std::unique_ptr<impl> pimpl;

    public:
        enum class node_kind
        {
            airport,
            navaid,
            fix,
        };

        // Fine-grained per-node classification. Drives the
        // per-waypoint cost modifier in the A* search. Layout
        // matters: `count` is used to size the modifier array and
        // ranges within the enum encode kind boundaries.
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

        // Coarse classification of an airway, derived from the
        // first one or two characters of the airway ID. Drives the
        // per-airway cost modifier when the use-airways toggle is
        // enabled.
        enum class awy_class : std::uint8_t
        {
            victor,   // V*
            jet,      // J*
            rnav,     // T*, Q*, TK*
            color,    // G*, A*, R*, B* (excluding AT/AR/BR/BF prefixes)
            other,    // AT, AR, BR, BF, PA, PR, anything else

            count
        };

        struct node
        {
            std::string id;
            node_kind kind;
            wp_subtype subtype;
            double lat;
            double lon;
        };

        struct airway_edge
        {
            std::size_t neighbor_index;
            std::string airway_id;
            awy_class type;
            // True when the underlying AWY_SEG row has
            // AWY_SEG_GAP_FLAG='Y' — a published discontinuity in
            // the airway. Cost factor is taken from
            // `options::gap_cost` instead of `awy_cost[type]`.
            bool is_gap;
        };

        // Build the routable-waypoint catalog and airway adjacency
        // by scanning APT_BASE / NAV_BASE / FIX_BASE / AWY_SEG.
        explicit route_planner(const nasr_database& db);
        ~route_planner();

        route_planner(const route_planner&) = delete;
        route_planner& operator=(const route_planner&) = delete;
        route_planner(route_planner&&) = delete;
        route_planner& operator=(route_planner&&) = delete;

        std::size_t node_count() const;

        // Fetch a node by its flat index. Throws std::out_of_range
        // if the index is invalid.
        const node& get_node(std::size_t index) const;

        // Lookup by canonical ID. Returns nullopt when the token does
        // not match any routable waypoint. Priority on collision is
        // airport > navaid > fix.
        std::optional<std::size_t> node_index(const std::string& id) const;

        // Airway neighbors of a node (both directions of each
        // AWY_SEG row are represented). Empty vector when the node
        // has no airway connections.
        const std::vector<airway_edge>& airway_neighbors(std::size_t index) const;

        // Route planning.

        // The four named cost levels are baked in. Numeric values
        // match g3xfplan: PREFER discounts, INCLUDE is neutral,
        // AVOID penalizes mildly, REJECT effectively excludes.
        static constexpr double cost_prefer  = 0.8;
        static constexpr double cost_include = 1.0;
        static constexpr double cost_avoid   = 1.25;
        static constexpr double cost_reject  = 1000.0;

        struct options
        {
            // Maximum direct-leg length, in nautical miles. A direct
            // step longer than this is multiplied by `cost_reject`.
            // Default matches g3xfplan.py's default.
            double max_leg_length_nm = 80.0;

            // Per-subtype cost modifier. Default-initialized to
            // `cost_include` so an options{} value is uniform-cost.
            std::array<double, static_cast<std::size_t>(wp_subtype::count)>
                wp_cost;

            // Per-airway-class cost modifier. Only applied when
            // `use_airways` is true; ignored otherwise.
            std::array<double, static_cast<std::size_t>(awy_class::count)>
                awy_cost;

            // Cost factor for an airway edge marked `is_gap`.
            // Applied in place of `awy_cost[type]` for that edge,
            // and only when `use_airways` is true. Default
            // `cost_include` (1.0) treats the bridge across a gap
            // as cost-neutral relative to a generic direct hop —
            // the airway's preference does *not* extend across the
            // discontinuity. Set to `cost_prefer` to make
            // gap-bridging cost-attractive (so a route that
            // follows a named airway including its gaps can win
            // over an alternative that switches airways).
            double gap_cost = cost_include;

            // When true: airway-routable navaid and fix subtypes
            // (VOR/VORTAC/VOR-DME/DME/NDB/NDB-DME and the WP/RP/CN/MR
            // fixes) are forced to `cost_include` regardless of their
            // configured `wp_cost`, and `awy_cost` modifies on-airway
            // edges. When false: `wp_cost` is honored verbatim and
            // airway edges receive no special treatment.
            bool use_airways = false;

            options();
        };

        // A pathfinding endpoint: either an existing graph node
        // (node_index < node_count()) or a synthetic lat/lon point
        // (node_index == synthetic). Synthetic endpoints do not
        // participate in airway adjacency; only their radius
        // neighbors are considered on the first/last hop.
        struct endpoint
        {
            std::size_t node_index;
            double lat;
            double lon;
        };
        static constexpr std::size_t synthetic =
            static_cast<std::size_t>(-1);

        // Plan a path between `origin` and `destination`. The
        // returned vector contains the graph-node indices of the
        // intermediate waypoints in order. Origin and destination
        // are NOT included — the caller splices them back in. An
        // empty result means a direct leg from origin to destination
        // satisfies `max_leg_length_nm`. Returns nullopt when no
        // viable path is found within that constraint.
        std::optional<std::vector<std::size_t>> plan_segment(
            const endpoint& origin, const endpoint& destination,
            const options& opts) const;

        // Convenience overload when both endpoints are graph nodes.
        std::optional<std::vector<std::size_t>> plan_segment(
            std::size_t origin_index, std::size_t destination_index,
            const options& opts) const;

        // Preprocessor: expand any `?` sigils in a route string using
        // A* and return a sigil-free route string suitable for
        // passing to flight_route. When the input contains no `?`,
        // the original text is returned unchanged.
        //
        // A `?` sits between two tokens, each of which is either a
        // point waypoint (airport ID, navaid ID, fix ID, or
        // DDMMSSXDDDMMSSY coordinate) or an airway ID. The two
        // cases produce different substitutions:
        //
        //   A ? B       point→point.  Plan A → B, emit the
        //               intermediates in place of '?'.
        //   A ? X, X ? A
        //   (X is an airway, A is a point) select the airway entry
        //               (or exit) fix via project-and-walk from A
        //               toward the airway's other-side waypoint,
        //               then plan A to that fix. Point and airway
        //               are emitted in the order they appear, with
        //               the chosen fix alongside the airway.
        //
        // Multiple sigils chain: `A ? B ? C` plans A→B then B→C.
        // Pattern `X ? Y` with airways on both sides of a single
        // sigil is rejected.
        //
        // Throws route_parse_error on invalid sigil placement or
        // when no path can be found.
        std::string expand_sigils(const std::string& text,
                                   const options& opts) const;
    };

} // namespace nasrbrowse
