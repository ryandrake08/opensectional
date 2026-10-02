#pragma once

#include "flight_route.hpp"
#include "route_plan_options.hpp"
#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace osect
{
    class elevation_source;
    class nasr_database;

    // A route string with its `?` sigils expanded. With terrain
    // avoidance on, `terrain_unchecked_nm` is the length of planned legs
    // whose corridor reached outside the terrain tree's coverage, where
    // terrain was not checked; otherwise it is 0.
    struct sigil_expansion
    {
        std::string text;
        double terrain_unchecked_nm = 0.0;
    };

    // A planned route, resolved, with sigil_expansion's
    // `terrain_unchecked_nm`.
    struct planned_route
    {
        flight_route route;
        double terrain_unchecked_nm = 0.0;
    };

    // Thrown by route_planner::expand_sigils and route_planner::parse
    // after route_planner::request_cancel.
    struct route_plan_cancelled : std::runtime_error
    {
        route_plan_cancelled() : std::runtime_error("route planning cancelled")
        {
        }
    };

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

        // Open the NASR database at `db_path` and build the
        // routable-waypoint catalog and airway adjacency by scanning
        // APT_BASE / NAV_BASE / FIX_BASE / AWY_SEG, and the SUA strata
        // for airspace avoidance. The user-waypoint database opens at
        // user_database::default_path() and the TFR database at
        // ephemeral_database::default_path(); the overload taking
        // their paths is for tests that need disposable ones. `terrain`
        // is read for terrain and airspace avoidance and must outlive
        // the planner.
        route_planner(const std::filesystem::path& db_path, const elevation_source& terrain);
        route_planner(const std::filesystem::path& db_path, const std::filesystem::path& user_db_path,
                      const std::filesystem::path& ephemeral_db_path, const elevation_source& terrain);
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
        // when no path can be found, and route_plan_cancelled at the
        // next A* step or sigil segment while a cancel is requested.
        sigil_expansion expand_sigils(const std::string& text, const options& opts) const;

        // Convenience: expand sigils and parse the result into a
        // flight_route resolved against the planner's database.
        // Equivalent to `flight_route(expand_sigils(text, opts).text,
        // db)` but doesn't expose the database. Throws route_parse_error
        // on sigil-grammar errors, A* failure, or token-resolution
        // failure, and route_plan_cancelled as expand_sigils does.
        planned_route parse(const std::string& text, const options& opts) const;

        // Makes a plan running on another thread, and any plan started
        // later, throw route_plan_cancelled until clear_cancel().
        void request_cancel();
        void clear_cancel();

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
            user,
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
        static constexpr std::size_t synthetic = static_cast<std::size_t>(-1);

        // Where a plan segment lies in the whole route, for the
        // terrain corridor's terminal tapers: the along-track distance
        // from the route's first waypoint to the segment's origin, and
        // from the segment's destination to the route's last waypoint
        // (a lower bound when later legs are still to be planned).
        struct route_ends
        {
            double before_origin_nm = 0.0;
            double after_destination_nm = 0.0;
        };

        std::size_t node_count() const;
        const node& get_node(std::size_t index) const;
        std::optional<std::size_t> node_index(const std::string& id) const;
        const std::vector<airway_edge>& airway_neighbors(std::size_t index) const;

        // Plan a path between `origin` and `destination`. The
        // returned vector contains the graph-node indices of the
        // intermediate waypoints in order. Origin and destination
        // are NOT included. Empty result means a direct leg
        // satisfies `max_leg_length_nm`. nullopt means no viable
        // path within that constraint. With `opts.avoid_terrain`, no
        // edge, including the direct leg and the final leg into
        // `destination`, fails the terrain test; throws
        // route_parse_error when there is no cruise altitude or no
        // terrain data. Throws route_plan_cancelled at an A* step
        // while a cancel is requested.
        std::optional<std::vector<std::size_t>> plan_segment(const endpoint& origin, const endpoint& destination,
                                                             const options& opts, const route_ends& ends) const;

        friend struct route_planner_test_access;
    };

    // Test-only proxy granting access to the catalog and A*
    // primitives. Defined inline so tests don't need a separate
    // translation unit; intended for use only by the route planner
    // tests.
    struct route_planner_test_access
    {
        using node_kind = route_planner::node_kind;
        using node = route_planner::node;
        using airway_edge = route_planner::airway_edge;
        using endpoint = route_planner::endpoint;
        using route_ends = route_planner::route_ends;
        static constexpr std::size_t synthetic = route_planner::synthetic;

        static std::size_t node_count(const route_planner& p)
        {
            return p.node_count();
        }
        static const node& get_node(const route_planner& p, std::size_t i)
        {
            return p.get_node(i);
        }
        static std::optional<std::size_t> node_index(const route_planner& p, const std::string& id)
        {
            return p.node_index(id);
        }
        static const std::vector<airway_edge>& airway_neighbors(const route_planner& p, std::size_t i)
        {
            return p.airway_neighbors(i);
        }
        static std::optional<std::vector<std::size_t>> plan_segment(const route_planner& p, const endpoint& origin,
                                                                    const endpoint& destination,
                                                                    const route_planner::options& opts,
                                                                    const route_ends& ends = {})
        {
            return p.plan_segment(origin, destination, opts, ends);
        }
    };

} // namespace osect
