#pragma once

#include "nasr_database.hpp"
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace osect
{
    // Stable identity for a saved route. Issued by user_database on
    // first persist (matches the user.db ROUTE row's primary key)
    // and carried by every layer that needs to talk about a
    // specific route — map_widget's public API, program.cpp's
    // tab_to_route map, etc. The render pipeline beneath map_widget
    // (feature_builder, feature_type, route_pick) still uses a
    // transient std::size_t into the routes vector; map_widget
    // translates id ↔ index at its public seam.
    using route_id = std::int64_t;

    // A coordinate literal — the return type of parse_latlon.
    struct latlon_ref
    {
        double lat;
        double lon;
    };

    // Parse a NASR-shorthand coordinate token (DDMMSSXDDDMMSSY) into
    // decimal degrees. Returns nullopt if the token is not in that
    // exact format. Used by the route parser and by
    // route_planner::expand_sigils to accept coordinate literals
    // wherever a point waypoint is expected.
    std::optional<latlon_ref> parse_latlon(const std::string& token);

    // Tag for a route waypoint's source type. These are the exact
    // strings stored in route_waypoint::kind and in the persisted DB
    // row — comparing against the named constant rather than a bare
    // literal keeps a typo a compile error.
    namespace waypoint_kind
    {
        inline constexpr const char* airport = "airport";
        inline constexpr const char* navaid = "navaid";
        inline constexpr const char* fix = "fix";
        inline constexpr const char* latlon = "latlon";
    }

    // A resolved waypoint on a route. Consumers only ever need the
    // identifier and coordinates, so that is all this carries — `id`
    // is the airport/navaid/fix identifier, empty for a latlon
    // waypoint whose identity is its coordinates.
    struct route_waypoint
    {
        std::string kind; // one of the waypoint_kind constants
        std::string id;
        double lat;
        double lon;
    };

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

    // One waypoint of a route in flat, persistable form. The database
    // stores a route as an ordered list of these; because each row
    // carries resolved coordinates, reconstructing a flight_route from
    // rows needs no nasr_database. `element_index` ties the row back
    // to the route_element it came from: consecutive rows sharing an
    // index whose `airway_id` is set form one airway traversal, while
    // a lone row with no `airway_id` is a standalone waypoint. Row
    // order is route order — `seq` is implicit in the vector index.
    struct route_waypoint_row
    {
        int element_index;
        std::string kind;       // "airport" | "navaid" | "fix" | "latlon"
        std::string identifier; // empty for "latlon"
        double lat;
        double lon;
        std::optional<std::string> airway_id;
    };

    // Thrown when a route string cannot be parsed or resolved
    struct route_parse_error : std::runtime_error
    {
        std::string token;
        int token_index;

        route_parse_error(const std::string& message, const std::string& token, int token_index)
            : std::runtime_error(message), token(token), token_index(token_index)
        {
        }

        route_parse_error(const std::string& message) : std::runtime_error(message), token_index(-1)
        {
        }
    };

    // One leg of an expanded route, from one waypoint to the next.
    struct route_leg
    {
        std::string from_id;
        std::string to_id;
        double distance_nm;
        double true_course_deg; // initial great-circle bearing, [0, 360)
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

        // Reconstruct a route from its persisted resolved form (see
        // route_waypoint_row). Touches no nasr_database — the rows
        // already carry resolved coordinates. Throws route_parse_error
        // if `rows` is empty or the element grouping is malformed.
        explicit flight_route(const std::vector<route_waypoint_row>& rows);

        // Reconstruct the shorthand text from the definition form
        std::string to_text() const;

        // The route in flat, persistable form: one row per waypoint in
        // each element, in route order. Round-trips through the row
        // constructor with no nasr_database.
        std::vector<route_waypoint_row> to_rows() const;

        // Insert a waypoint between expanded waypoints at `segment_index`
        // and `segment_index + 1`. Updates both elements and waypoints.
        // Re-runs airway_ize so any sequential runs of fixes collapse
        // back into shorthand form.
        void insert_waypoint(std::size_t segment_index, const route_waypoint& wp, const nasr_database& db);

        // Replace the waypoint at `waypoint_index` with `wp`. Flattens the
        // route into explicit waypoints, then re-runs airway_ize.
        void replace_waypoint(std::size_t waypoint_index, route_waypoint wp, const nasr_database& db);

        // Remove the waypoint at `waypoint_index`. Flattens the route,
        // then re-runs airway_ize. Callers must ensure the resulting
        // route still has >= 2 waypoints.
        void delete_waypoint(std::size_t waypoint_index, const nasr_database& db);

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
        void insert_waypoint_raw(std::size_t segment_index, const route_waypoint& wp);
    };

} // namespace osect
