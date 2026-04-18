#pragma once

#include "geo_types.hpp"
#include <vector>

namespace nasrbrowse
{
    // Number of line segments used to approximate a geodesic circle
    constexpr int GEODESIC_CIRCLE_SEGMENTS = 48;

    // Maximum great-circle distance (NM) before a line segment is
    // subdivided into shorter arcs
    constexpr double GEODESIC_INTERPOLATION_THRESHOLD_NM = 50.0;

    // Generate points on a geodesic circle (constant great-circle distance
    // from center). Returns n+1 points (closed ring).
    std::vector<airspace_point> geodesic_circle(double center_lat, double center_lon,
                                                 double radius_nm,
                                                 int n = GEODESIC_CIRCLE_SEGMENTS);

    // Subdivide a great-circle arc between two points so that no segment
    // exceeds max_segment_nm. Returns the full sequence including both
    // endpoints. If the arc is already short enough, returns just the
    // two endpoints.
    std::vector<airspace_point> geodesic_interpolate(double lat1, double lon1,
                                                      double lat2, double lon2,
                                                      double max_segment_nm = GEODESIC_INTERPOLATION_THRESHOLD_NM);

} // namespace nasrbrowse
