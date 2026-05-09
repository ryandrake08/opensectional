#pragma once

#include "nasr_database.hpp"
#include "tfr.hpp"
#include <cstddef>
#include <string>
#include <variant>
#include <vector>

namespace osect
{
    // A pick at a single point on a flight route — either a waypoint
    // or a leg between two waypoints. Carried as one alternative of
    // the `feature` variant so route entries flow through the same
    // pick / selection / popup machinery as chart features.
    struct route_pick
    {
        enum class part_kind : unsigned char
        {
            waypoint,
            leg
        };
        std::size_t route_index;
        part_kind part;
        // Waypoint index when part==waypoint, leg index (== from-
        // waypoint index) when part==leg.
        std::size_t inner_index;
        // Pre-computed display label like "Route 1: LIN (waypoint)"
        // or "Route 1: A - B (leg)". Filled in by route_type::pick at
        // pick time so summary() (which has no context argument) can
        // render the label without re-walking routes.
        std::string label;
    };

    using feature = std::variant<airport, navaid, fix, obstacle, class_airspace, sua, artcc, adiz, tfr, maa, pja, awos,
                                 comm_outlet, airway_segment, mtr_segment, runway, route_pick>;

    struct pick_result
    {
        double lon;
        double lat;
        std::vector<feature> features;
    };

} // namespace osect
