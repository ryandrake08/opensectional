#pragma once

#include "geo_types.hpp"
#include <vector>

namespace nasrbrowse
{
    // Number of line segments used to approximate a geodesic circle
    constexpr auto GEODESIC_CIRCLE_SEGMENTS = 48;

    // Maximum great-circle distance (NM) before a line segment is
    // subdivided into shorter arcs
    constexpr auto GEODESIC_INTERPOLATION_THRESHOLD_NM = 50.0;

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

    // Great-circle distance between two lat/lon points, in nautical
    // miles. Spherical-earth approximation.
    double haversine_nm(double lat1, double lon1, double lat2, double lon2);

    // Distance, in nautical miles, from point P to the nearest point
    // on the line segment from A to B. When P projects beyond an
    // endpoint, the returned distance is just the haversine to that
    // endpoint. Uses a local planar approximation — accurate to well
    // under 1% for segments of a few hundred nautical miles.
    double point_to_segment_distance_nm(double lat_a, double lon_a,
                                         double lat_b, double lon_b,
                                         double lat_p, double lon_p);

} // namespace nasrbrowse
