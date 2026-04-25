#pragma once

#include "nasr_database.hpp"
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace osect
{
    // Parse a NASR-shorthand coordinate token (DDMMSSXDDDMMSSY) into
    // decimal degrees. Returns nullopt if the token is not in that
    // exact format. Used by the route parser and by
    // route_planner::expand_sigils to accept coordinate literals
    // wherever a point waypoint is expected.
    struct latlon_ref;
    std::optional<latlon_ref> parse_latlon(const std::string& token);

    // A resolved waypoint on a route — one of the four point types
    struct airport_ref
    {
        std::string id;
        airport resolved;
    };
    struct navaid_ref
    {
        std::string id;
        navaid resolved;
    };
    struct fix_ref
    {
        std::string id;
        fix resolved;
    };
    struct latlon_ref
    {
        double lat;
        double lon;
    };

    using route_waypoint = std::variant<airport_ref, navaid_ref, fix_ref, latlon_ref>;

    double waypoint_lat(const route_waypoint& wp);
    double waypoint_lon(const route_waypoint& wp);
    std::string waypoint_id(const route_waypoint& wp);

    // An airway traversal: entry fix, exit fix, and the expanded sequence
    struct airway_ref
    {
        std::string airway_id;
        std::string entry_id;
        std::string exit_id;
        std::vector<route_waypoint> expanded;
    };

    // A route element is either a single waypoint or an airway traversal
    using route_element = std::variant<route_waypoint, airway_ref>;

    // Thrown when a route string cannot be parsed or resolved
    struct route_parse_error : std::runtime_error
    {
        std::string token;
        int token_index;

        route_parse_error(const std::string& message,
                          const std::string& token, int token_index)
            : std::runtime_error(message)
            , token(token)
            , token_index(token_index)
        {}

        route_parse_error(const std::string& message)
            : std::runtime_error(message)
            , token_index(-1)
        {}
    };

    // One leg of an expanded route, from one waypoint to the next.
    struct route_leg
    {
        std::string from_id;
        std::string to_id;
        double distance_nm;
        double true_course_deg;  // initial great-circle bearing, [0, 360)
    };

    // A parsed and resolved flight route. Always valid — construction
    // throws route_parse_error on failure.
    class flight_route
    {
    public:
        // The flat sequence of resolved waypoints — one per fix
        // along the expanded path. Read-only from outside; mutate
        // through insert_waypoint / replace_waypoint /
        // delete_waypoint so airway_ize can re-collapse the
        // shorthand.
        std::vector<route_waypoint> waypoints;

        // Parse a route string and resolve against the database.
        // The grammar is a sequence of space-separated tokens:
        //   - a waypoint ID (airport / navaid / fix), or
        //   - a DDMMSSXDDDMMSSY coordinate literal, or
        //   - an airway ID, which must appear as the middle of an
        //     `ENTRY AIRWAY EXIT` triple. ENTRY and EXIT are
        //     auto-corrected to the nearest published fix on the
        //     airway when necessary.
        // Throws route_parse_error on failure.
        flight_route(const std::string& text, const nasr_database& db);

        // Reconstruct the shorthand text from the definition form
        std::string to_text() const;

        // Insert a waypoint between expanded waypoints at `segment_index`
        // and `segment_index + 1`. Updates both elements and waypoints.
        // Re-runs airway_ize so any sequential runs of fixes collapse
        // back into shorthand form.
        void insert_waypoint(std::size_t segment_index,
                             const route_waypoint& wp,
                             const nasr_database& db);

        // Replace the waypoint at `waypoint_index` with `wp`. Flattens the
        // route into explicit waypoints, then re-runs airway_ize.
        void replace_waypoint(std::size_t waypoint_index, route_waypoint wp,
                              const nasr_database& db);

        // Remove the waypoint at `waypoint_index`. Flattens the route,
        // then re-runs airway_ize. Callers must ensure the resulting
        // route still has >= 2 waypoints.
        void delete_waypoint(std::size_t waypoint_index,
                             const nasr_database& db);

        // Per-leg distances and great-circle bearings between
        // consecutive waypoints. Empty for routes of fewer than two
        // waypoints (which are rejected by the constructor anyway).
        std::vector<route_leg> compute_legs() const;

        // Total great-circle distance from origin to destination,
        // in nautical miles.
        double total_distance_nm() const;

    private:
        // Internal definition-form sequence: a flat list of either
        // single waypoints or airway traversals. Reflects the
        // user-typed shorthand and the post-airway_ize compaction;
        // not part of the public API.
        std::vector<route_element> elements;

        // Collapse runs of 3+ consecutive waypoints that form a
        // sequential path along a common airway into airway_ref
        // elements. Called automatically by the constructor and by
        // the mutation methods.
        void airway_ize(const nasr_database& db);

        // The mutation work of `insert_waypoint`, separated so
        // that the public entry point only adds the assertions
        // and the airway_ize re-collapse.
        void insert_waypoint_raw(std::size_t segment_index,
                                  const route_waypoint& wp);
    };

} // namespace osect
