#include "geo_math.hpp"
#include <cmath>

namespace osect
{
    namespace
    {
        // WGS84 semi-major axis
        constexpr auto EARTH_RADIUS_NM = 6378137.0 / 1852.0;

        // Local equirectangular approximation: 1° of latitude ≈ 60 NM
        // everywhere; 1° of longitude is this scaled by cos(latitude).
        constexpr auto NM_PER_DEG = 60.0;
    }

    std::vector<airspace_point> geodesic_circle(double center_lat, double center_lon, double radius_nm, int n)
    {
        auto lat1 = center_lat * M_PI / 180.0;
        auto lon1 = center_lon * M_PI / 180.0;
        auto d = radius_nm / EARTH_RADIUS_NM;

        auto sin_lat1 = std::sin(lat1);
        auto cos_lat1 = std::cos(lat1);
        auto sin_d = std::sin(d);
        auto cos_d = std::cos(d);

        std::vector<airspace_point> pts;
        pts.reserve(n + 1);
        auto prev_lon = center_lon;
        for(int i = 0; i <= n; i++)
        {
            auto bearing = 2.0 * M_PI * i / n;
            auto lat2 = std::asin(sin_lat1 * cos_d + cos_lat1 * sin_d * std::cos(bearing));
            auto lon2 = lon1 + std::atan2(std::sin(bearing) * sin_d * cos_lat1, cos_d - sin_lat1 * std::sin(lat2));
            auto lat_deg = lat2 * 180.0 / M_PI;
            auto lon_deg = lon2 * 180.0 / M_PI;

            auto delta = lon_deg - prev_lon;
            if(delta > 180.0)
            {
                lon_deg -= 360.0;
            }
            else if(delta < -180.0)
            {
                lon_deg += 360.0;
            }
            prev_lon = lon_deg;

            pts.push_back({lat_deg, lon_deg});
        }
        return pts;
    }

    double haversine_distance_nm(double lat1, double lon1, double lat2, double lon2)
    {
        auto rlat1 = lat1 * M_PI / 180.0;
        auto rlat2 = lat2 * M_PI / 180.0;
        auto dlat = rlat2 - rlat1;
        auto dlon = (lon2 - lon1) * M_PI / 180.0;
        auto a = std::sin(dlat * 0.5) * std::sin(dlat * 0.5) +
                 std::cos(rlat1) * std::cos(rlat2) * std::sin(dlon * 0.5) * std::sin(dlon * 0.5);
        return 2.0 * std::atan2(std::sqrt(a), std::sqrt(1.0 - a)) * EARTH_RADIUS_NM;
    }

    double equirectangular_distance_nm(double lat1, double lon1, double lat2, double lon2)
    {
        // Project onto a plane tangent at the midpoint latitude, so the
        // result is symmetric in the two points.
        auto nm_per_deg_lon = NM_PER_DEG * std::cos((lat1 + lat2) * 0.5 * M_PI / 180.0);
        auto dlat = (lat2 - lat1) * NM_PER_DEG;
        auto dlon = (lon2 - lon1) * nm_per_deg_lon;
        return std::sqrt(dlat * dlat + dlon * dlon);
    }

    double true_course_deg(double lat1, double lon1, double lat2, double lon2)
    {
        auto rlat1 = lat1 * M_PI / 180.0;
        auto rlat2 = lat2 * M_PI / 180.0;
        auto dlon = (lon2 - lon1) * M_PI / 180.0;
        auto y = std::sin(dlon) * std::cos(rlat2);
        auto x = std::cos(rlat1) * std::sin(rlat2) - std::sin(rlat1) * std::cos(rlat2) * std::cos(dlon);
        auto brg = std::atan2(y, x) * 180.0 / M_PI;
        if(brg < 0.0)
        {
            brg += 360.0;
        }
        return brg;
    }

    double cross_track_nm(double lat1, double lon1, double lat2, double lon2, double lat_p, double lon_p)
    {
        auto d13 = haversine_distance_nm(lat1, lon1, lat_p, lon_p);
        if(d13 == 0.0)
        {
            return 0.0;
        }
        auto rla1 = lat1 * M_PI / 180.0;
        auto rla2 = lat2 * M_PI / 180.0;
        auto rla3 = lat_p * M_PI / 180.0;
        auto dlon12 = (lon2 - lon1) * M_PI / 180.0;
        auto dlon13 = (lon_p - lon1) * M_PI / 180.0;
        auto b12 = std::atan2(std::sin(dlon12) * std::cos(rla2),
                              std::cos(rla1) * std::sin(rla2) - std::sin(rla1) * std::cos(rla2) * std::cos(dlon12));
        auto b13 = std::atan2(std::sin(dlon13) * std::cos(rla3),
                              std::cos(rla1) * std::sin(rla3) - std::sin(rla1) * std::cos(rla3) * std::cos(dlon13));
        return std::asin(std::sin(d13 / EARTH_RADIUS_NM) * std::sin(b13 - b12)) * EARTH_RADIUS_NM;
    }

    double point_to_segment_distance_nm(double lat_a, double lon_a, double lat_b, double lon_b, double lat_p,
                                        double lon_p)
    {
        // Local equirectangular projection about the segment midpoint.
        // Using a single cos(lat) factor is accurate to a small
        // fraction of a percent for segment lengths of a few hundred
        // nautical miles.
        auto mid_lat = (lat_a + lat_b) * 0.5;
        auto nm_per_deg_lon = NM_PER_DEG * std::cos(mid_lat * M_PI / 180.0);

        auto bx = (lon_b - lon_a) * nm_per_deg_lon;
        auto by = (lat_b - lat_a) * NM_PER_DEG;
        auto px = (lon_p - lon_a) * nm_per_deg_lon;
        auto py = (lat_p - lat_a) * NM_PER_DEG;

        auto ab_len_sq = bx * bx + by * by;
        if(ab_len_sq == 0.0)
        {
            return std::sqrt(px * px + py * py);
        }

        auto t = (px * bx + py * by) / ab_len_sq;
        if(t < 0.0)
        {
            t = 0.0;
        }
        else if(t > 1.0)
        {
            t = 1.0;
        }

        auto fx = t * bx;
        auto fy = t * by;
        return std::sqrt((px - fx) * (px - fx) + (py - fy) * (py - fy));
    }

    std::vector<airspace_point> geodesic_interpolate(double lat1, double lon1, double lat2, double lon2,
                                                     double max_segment_nm)
    {
        auto rlat1 = lat1 * M_PI / 180.0;
        auto rlon1 = lon1 * M_PI / 180.0;
        auto rlat2 = lat2 * M_PI / 180.0;
        auto rlon2 = lon2 * M_PI / 180.0;

        auto dlat = rlat2 - rlat1;
        auto dlon = rlon2 - rlon1;
        auto a = std::sin(dlat * 0.5) * std::sin(dlat * 0.5) +
                 std::cos(rlat1) * std::cos(rlat2) * std::sin(dlon * 0.5) * std::sin(dlon * 0.5);
        auto d = 2.0 * std::atan2(std::sqrt(a), std::sqrt(1.0 - a));
        auto dist_nm = d * EARTH_RADIUS_NM;

        auto segments = static_cast<int>(std::ceil(dist_nm / max_segment_nm));
        if(segments <= 1)
        {
            return {{lat1, lon1}, {lat2, lon2}};
        }

        auto sin_d = std::sin(d);
        std::vector<airspace_point> pts;
        pts.reserve(segments + 1);
        auto prev_lon = lon1;
        for(int i = 0; i <= segments; i++)
        {
            auto f = static_cast<double>(i) / segments;
            auto A = std::sin((1.0 - f) * d) / sin_d;
            auto B = std::sin(f * d) / sin_d;
            auto x = A * std::cos(rlat1) * std::cos(rlon1) + B * std::cos(rlat2) * std::cos(rlon2);
            auto y = A * std::cos(rlat1) * std::sin(rlon1) + B * std::cos(rlat2) * std::sin(rlon2);
            auto z = A * std::sin(rlat1) + B * std::sin(rlat2);
            auto lat = std::atan2(z, std::sqrt(x * x + y * y)) * 180.0 / M_PI;
            auto lon = std::atan2(y, x) * 180.0 / M_PI;

            // Keep longitude continuous across the antimeridian
            auto delta = lon - prev_lon;
            if(delta > 180.0)
            {
                lon -= 360.0;
            }
            else if(delta < -180.0)
            {
                lon += 360.0;
            }
            prev_lon = lon;

            pts.push_back({lat, lon});
        }
        return pts;
    }

    airspace_point geodesic_point(double lat1, double lon1, double lat2, double lon2, double fraction)
    {
        auto rlat1 = lat1 * M_PI / 180.0;
        auto rlon1 = lon1 * M_PI / 180.0;
        auto rlat2 = lat2 * M_PI / 180.0;
        auto rlon2 = lon2 * M_PI / 180.0;

        auto dlat = rlat2 - rlat1;
        auto dlon = rlon2 - rlon1;
        auto a = std::sin(dlat * 0.5) * std::sin(dlat * 0.5) +
                 std::cos(rlat1) * std::cos(rlat2) * std::sin(dlon * 0.5) * std::sin(dlon * 0.5);
        auto d = 2.0 * std::atan2(std::sqrt(a), std::sqrt(1.0 - a));
        if(d == 0.0)
        {
            return {lat1, lon1};
        }

        auto sin_d = std::sin(d);
        auto A = std::sin((1.0 - fraction) * d) / sin_d;
        auto B = std::sin(fraction * d) / sin_d;
        auto x = A * std::cos(rlat1) * std::cos(rlon1) + B * std::cos(rlat2) * std::cos(rlon2);
        auto y = A * std::cos(rlat1) * std::sin(rlon1) + B * std::cos(rlat2) * std::sin(rlon2);
        auto z = A * std::sin(rlat1) + B * std::sin(rlat2);
        auto lat = std::atan2(z, std::sqrt(x * x + y * y)) * 180.0 / M_PI;
        auto lon = std::atan2(y, x) * 180.0 / M_PI;
        if(lon - lon1 > 180.0)
        {
            lon -= 360.0;
        }
        else if(lon - lon1 < -180.0)
        {
            lon += 360.0;
        }
        return {lat, lon};
    }

    double nm_to_deg_lon(double nm, double lat)
    {
        auto cos_lat = std::cos(lat * M_PI / 180.0);
        return (cos_lat > 1e-6) ? nm / (NM_PER_DEG * cos_lat) : 180.0;
    }

    geo_bbox bbox_around(double lat, double lon, double radius_nm)
    {
        auto dlat = radius_nm / NM_PER_DEG;
        auto dlon = nm_to_deg_lon(radius_nm, lat);
        return {lon - dlon, lat - dlat, lon + dlon, lat + dlat};
    }

} // namespace osect
