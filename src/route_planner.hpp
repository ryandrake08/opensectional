#pragma once

#include "route_plan_options.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace osect
{
    class nasr_database;

    // Test-access proxy declared below. `friend` permission lets
    // test_route_planner reach catalog and A* internals without
    // putting them in the public API.
    struct route_planner_test_access;

    // A* pathfinder over the NASR waypoint graph. Constructed once
    // from the database; holds an immutable in-memory catalog of
    // routable waypoints plus an airway adjacency map.
    //
    // Public API is intentionally thin: callers feed text through
    // `expand_sigils` and use the returned sigil-free string as
    // `flight_route` input. The catalog and A* primitives that
    // back this method are exposed only to the test proxy.
    class route_planner
    {
        struct impl;
        std::unique_ptr<impl> pimpl;

    public:
        // Backward-compatible alias so existing references to
        // `route_planner::options` keep working.
        using options = route_plan_options;

        // Build the routable-waypoint catalog and airway adjacency
        // by scanning APT_BASE / NAV_BASE / FIX_BASE / AWY_SEG.
        explicit route_planner(const nasr_database& db);
        ~route_planner();

        route_planner(const route_planner&) = delete;
        route_planner& operator=(const route_planner&) = delete;
        route_planner(route_planner&&) = delete;
        route_planner& operator=(route_planner&&) = delete;

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

    private:
        // ---- Catalog and A* primitives ----
        //
        // Hidden from the public API. Tests reach them through
        // `route_planner_test_access`; the planner's own
        // `expand_sigils` calls them directly.

        enum class node_kind
        {
            airport,
            navaid,
            fix,
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
            bool is_gap;
        };

        // A pathfinding endpoint: either an existing graph node
        // (node_index < node_count()) or a synthetic lat/lon point
        // (node_index == synthetic).
        struct endpoint
        {
            std::size_t node_index;
            double lat;
            double lon;
        };
        static constexpr std::size_t synthetic =
            static_cast<std::size_t>(-1);

        std::size_t node_count() const;
        const node& get_node(std::size_t index) const;
        std::optional<std::size_t> node_index(const std::string& id) const;
        const std::vector<airway_edge>& airway_neighbors(std::size_t index) const;

        // Plan a path between `origin` and `destination`. The
        // returned vector contains the graph-node indices of the
        // intermediate waypoints in order. Origin and destination
        // are NOT included. Empty result means a direct leg
        // satisfies `max_leg_length_nm`. nullopt means no viable
        // path within that constraint.
        std::optional<std::vector<std::size_t>> plan_segment(
            const endpoint& origin, const endpoint& destination,
            const options& opts) const;

        friend struct route_planner_test_access;
    };

    // Test-only proxy granting access to the catalog and A*
    // primitives. Defined inline so tests don't need a separate
    // translation unit; intended for use only by
    // tests/test_route_planner.cpp.
    struct route_planner_test_access
    {
        using node_kind = route_planner::node_kind;
        using node = route_planner::node;
        using airway_edge = route_planner::airway_edge;
        using endpoint = route_planner::endpoint;
        static constexpr std::size_t synthetic = route_planner::synthetic;

        static std::size_t node_count(const route_planner& p)
        {
            return p.node_count();
        }
        static const node& get_node(const route_planner& p, std::size_t i)
        {
            return p.get_node(i);
        }
        static std::optional<std::size_t>
        node_index(const route_planner& p, const std::string& id)
        {
            return p.node_index(id);
        }
        static const std::vector<airway_edge>&
        airway_neighbors(const route_planner& p, std::size_t i)
        {
            return p.airway_neighbors(i);
        }
        static std::optional<std::vector<std::size_t>>
        plan_segment(const route_planner& p,
                      const endpoint& origin, const endpoint& destination,
                      const route_planner::options& opts)
        {
            return p.plan_segment(origin, destination, opts);
        }
    };

} // namespace osect
