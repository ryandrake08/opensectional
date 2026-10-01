#pragma once

#include "geo_types.hpp"
#include <optional>
#include <vector>

namespace osect
{
    class elevation_source;
    class nasr_database;

    // A point on a route's centreline. `distance_nm` is along-track from
    // the route start; longitudes are continuous between adjacent
    // stations and may lie outside [-180, 180].
    struct corridor_station
    {
        double distance_nm = 0.0;
        geo_point point;
        double half_width_nm = 0.0;
    };

    // Maxima in the corridor swept between two adjacent stations: the
    // union of the boxes of each station's half-width around it. Empty
    // when the corridor has no height data, or no obstacles. `covered` is
    // true when the corridor lies inside the terrain tree's coverage
    // (elevation_source::covers). `window` is the box the maxima cover.
    struct corridor_interval
    {
        geo_bbox window;
        std::optional<double> terrain_ft;
        std::optional<double> obstacle_ft;
        bool covered = false;
    };

    // Corridor half-width at a route position: half of
    // `corridor_width_nm`, narrowing linearly to zero within
    // TERRAIN_PROFILE_TERMINAL_CORRIDOR_DISTANCE_NM of either route end.
    double corridor_half_width_nm(double corridor_width_nm, double distance_from_start_nm, double distance_to_end_nm);

    // One corridor_interval per pair of adjacent stations. Terrain
    // maxima come from elevation_source::maximum_elevation_ft and never
    // under-report. Obstacles are DOF AMSL heights, fetched for runs of
    // intervals spanning at most TERRAIN_PROFILE_OBSTACLE_QUERY_SPAN_NM;
    // with `include_obstacles` false, `obstacle_ft` is always empty.
    std::vector<corridor_interval> corridor_intervals(const std::vector<corridor_station>& stations,
                                                      const elevation_source& terrain, const nasr_database& obstacles,
                                                      bool include_obstacles);
} // namespace osect
