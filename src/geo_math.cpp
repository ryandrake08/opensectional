#include "geo_math.hpp"
#include <cmath>

namespace nasrbrowse
{
    namespace
    {
        // WGS84 semi-major axis
        constexpr auto EARTH_RADIUS_NM = 6378137.0 / 1852.0;
    }

    std::vector<airspace_point> geodesic_circle(double center_lat, double center_lon,
                                                double radius_nm, int n)
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
            if(delta > 180.0) lon_deg -= 360.0;
            else if(delta < -180.0) lon_deg += 360.0;
            prev_lon = lon_deg;

            pts.push_back({lat_deg, lon_deg});
        }
        return pts;
    }

    std::vector<airspace_point> geodesic_interpolate(double lat1, double lon1,
                                                     double lat2, double lon2,
                                                     double max_segment_nm)
    {
        auto rlat1 = lat1 * M_PI / 180.0;
        auto rlon1 = lon1 * M_PI / 180.0;
        auto rlat2 = lat2 * M_PI / 180.0;
        auto rlon2 = lon2 * M_PI / 180.0;

        auto dlat = rlat2 - rlat1;
        auto dlon = rlon2 - rlon1;
        auto a = std::sin(dlat * 0.5) * std::sin(dlat * 0.5) +
                 std::cos(rlat1) * std::cos(rlat2) *
                 std::sin(dlon * 0.5) * std::sin(dlon * 0.5);
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
            if(delta > 180.0) lon -= 360.0;
            else if(delta < -180.0) lon += 360.0;
            prev_lon = lon;

            pts.push_back({lat, lon});
        }
        return pts;
    }

} // namespace nasrbrowse
