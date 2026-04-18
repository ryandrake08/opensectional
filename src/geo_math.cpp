#include "geo_math.hpp"
#include <cmath>

namespace nasrbrowse
{
    namespace
    {
        // WGS84 semi-major axis
        constexpr double EARTH_RADIUS_NM = 6378137.0 / 1852.0;
    }

    std::vector<airspace_point> geodesic_circle(double center_lat, double center_lon,
                                                double radius_nm, int n)
    {
        double lat1 = center_lat * M_PI / 180.0;
        double lon1 = center_lon * M_PI / 180.0;
        double d = radius_nm / EARTH_RADIUS_NM;

        double sin_lat1 = std::sin(lat1);
        double cos_lat1 = std::cos(lat1);
        double sin_d = std::sin(d);
        double cos_d = std::cos(d);

        std::vector<airspace_point> pts;
        pts.reserve(n + 1);
        double prev_lon = center_lon;
        for(int i = 0; i <= n; i++)
        {
            double bearing = 2.0 * M_PI * i / n;
            double lat2 = std::asin(sin_lat1 * cos_d + cos_lat1 * sin_d * std::cos(bearing));
            double lon2 = lon1 + std::atan2(std::sin(bearing) * sin_d * cos_lat1, cos_d - sin_lat1 * std::sin(lat2));
            double lat_deg = lat2 * 180.0 / M_PI;
            double lon_deg = lon2 * 180.0 / M_PI;

            double delta = lon_deg - prev_lon;
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
        double rlat1 = lat1 * M_PI / 180.0;
        double rlon1 = lon1 * M_PI / 180.0;
        double rlat2 = lat2 * M_PI / 180.0;
        double rlon2 = lon2 * M_PI / 180.0;

        double dlat = rlat2 - rlat1;
        double dlon = rlon2 - rlon1;
        double a = std::sin(dlat * 0.5) * std::sin(dlat * 0.5) +
                   std::cos(rlat1) * std::cos(rlat2) *
                   std::sin(dlon * 0.5) * std::sin(dlon * 0.5);
        double d = 2.0 * std::atan2(std::sqrt(a), std::sqrt(1.0 - a));
        double dist_nm = d * EARTH_RADIUS_NM;

        int segments = static_cast<int>(std::ceil(dist_nm / max_segment_nm));
        if(segments <= 1)
        {
            return {{lat1, lon1}, {lat2, lon2}};
        }

        double sin_d = std::sin(d);
        std::vector<airspace_point> pts;
        pts.reserve(segments + 1);
        double prev_lon = lon1;
        for(int i = 0; i <= segments; i++)
        {
            double f = static_cast<double>(i) / segments;
            double A = std::sin((1.0 - f) * d) / sin_d;
            double B = std::sin(f * d) / sin_d;
            double x = A * std::cos(rlat1) * std::cos(rlon1) + B * std::cos(rlat2) * std::cos(rlon2);
            double y = A * std::cos(rlat1) * std::sin(rlon1) + B * std::cos(rlat2) * std::sin(rlon2);
            double z = A * std::sin(rlat1) + B * std::sin(rlat2);
            double lat = std::atan2(z, std::sqrt(x * x + y * y)) * 180.0 / M_PI;
            double lon = std::atan2(y, x) * 180.0 / M_PI;

            // Keep longitude continuous across the antimeridian
            double delta = lon - prev_lon;
            if(delta > 180.0) lon -= 360.0;
            else if(delta < -180.0) lon += 360.0;
            prev_lon = lon;

            pts.push_back({lat, lon});
        }
        return pts;
    }

} // namespace nasrbrowse
