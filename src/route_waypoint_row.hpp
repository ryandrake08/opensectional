#pragma once

#include <optional>
#include <string>

namespace osect
{
    // One waypoint of a route in flat, persistable form. The database
    // stores a route as an ordered list of these; because each row
    // carries resolved coordinates, reconstructing a flight_route from
    // rows needs no nasr_database. `element_index` ties the row back
    // to the route_element it came from: consecutive rows sharing an
    // index whose `airway_id` is set form one airway traversal, while
    // a lone row with no `airway_id` is a standalone waypoint. Row
    // order is route order — `seq` is implicit in the vector index.
    //
    // Its own header (rather than flight_route.hpp) so user_database —
    // which only needs this row type — does not pull in the route
    // model and nasr_database.
    struct route_waypoint_row
    {
        int element_index;
        std::string kind;       // "airport" | "navaid" | "fix" | "latlon"
        std::string identifier; // empty for "latlon"
        double lat;
        double lon;
        std::optional<std::string> airway_id;
    };
}
