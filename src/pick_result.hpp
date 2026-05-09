#pragma once

#include "nasr_database.hpp"
#include "tfr.hpp"
#include <cstddef>
#include <variant>
#include <vector>

namespace osect
{
    using feature = std::variant<airport, navaid, fix, obstacle, class_airspace, sua, artcc, adiz, tfr, maa, pja, awos,
                                 comm_outlet, airway_segment, mtr_segment, runway>;

    struct pick_result
    {
        double lon;
        double lat;
        std::vector<feature> features;
    };

    // A pick at a single point on a flight route — either a waypoint
    // or a leg between two waypoints. Used in the multi-hit pick
    // selector when a click lands where multiple routes (and possibly
    // a feature) overlap.
    struct route_pick_item
    {
        enum class kind : unsigned char
        {
            waypoint,
            leg
        };
        std::size_t route_index;
        kind k;
        // Waypoint index when k==waypoint, leg index (== from-waypoint
        // index) when k==leg.
        std::size_t inner_index;
    };

    // A single candidate in a pick selector. Either an underlying
    // chart feature (airport, navaid, ...) or a position on a flight
    // route (waypoint or leg).
    using pick_item = std::variant<feature, route_pick_item>;

} // namespace osect
