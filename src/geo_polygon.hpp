#pragma once

#include "geo_types.hpp"
#include <algorithm>
#include <cstddef>
#include <vector>

// Planar polygon tests in raw lon/lat degrees (lon as x, lat as y).

namespace osect
{
    // Even-odd ray cast of point (px, py) against a closed or open ring.
    inline bool point_in_ring(double px, double py, const std::vector<geo_point>& ring)
    {
        auto inside = false;
        for(std::size_t i = 0, j = ring.size() - 1; i < ring.size(); j = i++)
        {
            auto yi = ring[i].lat;
            auto yj = ring[j].lat;
            auto xi = ring[i].lon;
            auto xj = ring[j].lon;
            if(((yi > py) != (yj > py)) && (px < (xj - xi) * (py - yi) / (yj - yi) + xi))
            {
                inside = !inside;
            }
        }
        return inside;
    }

    // `Ring` is any type with `points` and `is_hole` (polygon_ring, sua_ring).
    // A point inside any hole is outside.
    template <typename Ring>
    bool point_in_multi_ring(double lon, double lat, const std::vector<Ring>& rings)
    {
        auto inside = false;
        for(const auto& ring : rings)
        {
            if(point_in_ring(lon, lat, ring.points))
            {
                if(ring.is_hole)
                {
                    inside = false;
                    break;
                }
                inside = true;
            }
        }
        return inside;
    }

    // True when segments a0-a1 and b0-b1 share at least one point,
    // including touching endpoints and collinear overlap.
    inline bool segments_intersect(const geo_point& a0, const geo_point& a1, const geo_point& b0, const geo_point& b1)
    {
        // Sign of the cross product (q - p) x (r - p): which side of
        // line p-q point r lies on.
        auto orientation = [](const geo_point& p, const geo_point& q, const geo_point& r)
        {
            const double cross = (q.lon - p.lon) * (r.lat - p.lat) - (q.lat - p.lat) * (r.lon - p.lon);
            return (cross > 0.0) - (cross < 0.0);
        };
        // For r collinear with p-q: true when r lies within their bbox.
        auto on_segment = [](const geo_point& p, const geo_point& q, const geo_point& r)
        {
            return std::min(p.lon, q.lon) <= r.lon && r.lon <= std::max(p.lon, q.lon) &&
                   std::min(p.lat, q.lat) <= r.lat && r.lat <= std::max(p.lat, q.lat);
        };
        const int o1 = orientation(a0, a1, b0);
        const int o2 = orientation(a0, a1, b1);
        const int o3 = orientation(b0, b1, a0);
        const int o4 = orientation(b0, b1, a1);
        if(o1 != o2 && o3 != o4)
        {
            return true;
        }
        return (o1 == 0 && on_segment(a0, a1, b0)) || (o2 == 0 && on_segment(a0, a1, b1)) ||
               (o3 == 0 && on_segment(b0, b1, a0)) || (o4 == 0 && on_segment(b0, b1, a1));
    }

    // True when segment a-b has any point inside the multi-ring area:
    // an end lies inside it, or the segment meets any ring's boundary.
    template <typename Ring>
    bool segment_meets_multi_ring(const geo_point& a, const geo_point& b, const std::vector<Ring>& rings)
    {
        if(point_in_multi_ring(a.lon, a.lat, rings) || point_in_multi_ring(b.lon, b.lat, rings))
        {
            return true;
        }
        for(const auto& ring : rings)
        {
            const auto& pts = ring.points;
            for(std::size_t i = 0, j = pts.size() - 1; i < pts.size(); j = i++)
            {
                if(segments_intersect(a, b, pts[j], pts[i]))
                {
                    return true;
                }
            }
        }
        return false;
    }

} // namespace osect
