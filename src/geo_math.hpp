#pragma once

#include "geo_types.hpp"
#include <limits>
#include <vector>

namespace osect
{
    // Number of line segments used to approximate a geodesic circle
    constexpr auto GEODESIC_CIRCLE_SEGMENTS = 48;

    // Maximum great-circle distance (NM) before a line segment is
    // subdivided into shorter arcs
    constexpr auto GEODESIC_INTERPOLATION_THRESHOLD_NM = 50.0;

    // Generate points on a geodesic circle (constant great-circle distance
    // from center). Returns n+1 points (closed ring).
    std::vector<airspace_point> geodesic_circle(double center_lat, double center_lon, double radius_nm,
                                                int n = GEODESIC_CIRCLE_SEGMENTS);

    // Subdivide a great-circle arc between two points so that no segment
    // exceeds max_segment_nm. Returns the full sequence including both
    // endpoints. If the arc is already short enough, returns just the
    // two endpoints.
    std::vector<airspace_point> geodesic_interpolate(double lat1, double lon1, double lat2, double lon2,
                                                     double max_segment_nm = GEODESIC_INTERPOLATION_THRESHOLD_NM);

    // Great-circle distance between two lat/lon points, in nautical
    // miles. Spherical-earth approximation.
    double haversine_distance_nm(double lat1, double lon1, double lat2, double lon2);

    // Distance between two lat/lon points, in nautical miles, via the
    // local equirectangular ("flat earth at this latitude")
    // approximation. Cheaper than haversine_distance_nm — one cosine
    // instead of several transcendentals — and accurate to a small
    // fraction of a percent over the few-hundred-NM spans used for
    // hit-testing.
    double equirectangular_distance_nm(double lat1, double lon1, double lat2, double lon2);

    // Iterator to the element of `candidates` whose (lat, lon) is the
    // shortest great-circle distance from (lat, lon), or candidates.end()
    // when `candidates` is empty. Each element must expose `lat` and
    // `lon` members in degrees.
    template <typename T>
    typename std::vector<T>::const_iterator nearest_to(const std::vector<T>& candidates, double lat, double lon)
    {
        auto best = candidates.end();
        auto best_d = std::numeric_limits<double>::infinity();
        for(auto it = candidates.begin(); it != candidates.end(); ++it)
        {
            auto d = haversine_distance_nm(lat, lon, it->lat, it->lon);
            if(d < best_d)
            {
                best_d = d;
                best = it;
            }
        }
        return best;
    }

    // Initial great-circle bearing from (lat1, lon1) to (lat2, lon2), in
    // degrees, normalized to [0, 360).
    double true_course_deg(double lat1, double lon1, double lat2, double lon2);

    // Distance, in nautical miles, from point P to the nearest point
    // on the line segment from A to B. When P projects beyond an
    // endpoint, the returned distance is just the haversine to that
    // endpoint. Uses a local planar approximation — accurate to well
    // under 1% for segments of a few hundred nautical miles.
    double point_to_segment_distance_nm(double lat_a, double lon_a, double lat_b, double lon_b, double lat_p,
                                        double lon_p);

    // Signed cross-track distance in NM from point (lat_p, lon_p) to the
    // great circle through (lat1, lon1) and (lat2, lon2). Absolute value
    // is the perpendicular distance from the point to the great circle.
    double cross_track_nm(double lat1, double lon1, double lat2, double lon2, double lat_p, double lon_p);

    // Longitude degrees spanned by `nm` nautical miles at the given
    // latitude, via the local equirectangular approximation. Clamped to
    // 180 near the poles where the cosine of the latitude vanishes.
    double nm_to_deg_lon(double nm, double lat);

    // Approximate bounding box of `radius_nm` around (lat, lon). Over-
    // estimates near the poles and across the antimeridian; callers that
    // need exactness must still filter results by true distance.
    geo_bbox bbox_around(double lat, double lon, double radius_nm);

} // namespace osect
