#pragma once

#include <cmath>

// Web Mercator (EPSG:3857) map view with pan/zoom
//
// World coordinates: Web Mercator meters
//   x: -20037508.34 (180W) to 20037508.34 (180E)
//   y: -20037508.34 (85S)  to 20037508.34 (85N)
//
// Tile coordinates: standard XYZ (z/x/y)
//   At zoom z: 2^z tiles in each dimension
//   Origin at top-left (lon=-180, lat=85.05)

namespace nasrbrowse
{
    // Maximum latitude for Web Mercator (atan(sinh(pi)) in degrees)
    constexpr double MAX_LATITUDE = 85.0511287798;

    // Earth radius in meters (WGS84 semi-major axis)
    constexpr double EARTH_RADIUS = 6378137.0;

    // Half the circumference in meters
    constexpr double HALF_CIRCUMFERENCE = M_PI * EARTH_RADIUS; // ~20037508.34

    // Convert longitude (degrees) to Web Mercator x (meters)
    inline double lon_to_mx(double lon)
    {
        return lon * HALF_CIRCUMFERENCE / 180.0;
    }

    // Convert latitude (degrees) to Web Mercator y (meters)
    inline double lat_to_my(double lat)
    {
        double lat_rad = lat * M_PI / 180.0;
        double y = std::log(std::tan(M_PI / 4.0 + lat_rad / 2.0));
        return y * EARTH_RADIUS;
    }

    // Convert Web Mercator x (meters) to longitude (degrees)
    inline double mx_to_lon(double mx)
    {
        return mx * 180.0 / HALF_CIRCUMFERENCE;
    }

    // Convert Web Mercator y (meters) to latitude (degrees)
    inline double my_to_lat(double my)
    {
        double y = my / EARTH_RADIUS;
        double lat_rad = 2.0 * std::atan(std::exp(y)) - M_PI / 2.0;
        return lat_rad * 180.0 / M_PI;
    }

    // Convert lon/lat to tile coordinates at given zoom
    inline void lonlat_to_tile(double lon, double lat, int zoom, int& tx, int& ty)
    {
        double n = std::pow(2.0, zoom);
        tx = static_cast<int>((lon + 180.0) / 360.0 * n);
        double lat_rad = lat * M_PI / 180.0;
        ty = static_cast<int>((1.0 - std::log(std::tan(lat_rad) + 1.0 / std::cos(lat_rad)) / M_PI) / 2.0 * n);
    }

    // Convert tile coordinates to lon/lat (top-left corner of tile)
    inline void tile_to_lonlat(int tx, int ty, int zoom, double& lon, double& lat)
    {
        double n = std::pow(2.0, zoom);
        lon = tx / n * 360.0 - 180.0;
        double lat_rad = std::atan(std::sinh(M_PI * (1.0 - 2.0 * ty / n)));
        lat = lat_rad * 180.0 / M_PI;
    }

    // Compute zoom level from view extent and viewport size
    inline double zoom_level(double half_extent_y, int viewport_height)
    {
        double meters_per_pixel = (half_extent_y * 2.0) / viewport_height;
        double world_size = 2.0 * HALF_CIRCUMFERENCE;
        return std::log2(world_size / (256.0 * meters_per_pixel));
    }

    // Get Web Mercator bounds (meters) for a tile
    inline void tile_bounds_meters(int tx, int ty, int zoom,
                                   double& x_min, double& y_min,
                                   double& x_max, double& y_max)
    {
        double n = std::pow(2.0, zoom);
        double tile_size = 2.0 * HALF_CIRCUMFERENCE / n;
        x_min = -HALF_CIRCUMFERENCE + tx * tile_size;
        x_max = x_min + tile_size;
        // Y is flipped: tile y=0 is at top (north)
        y_max = HALF_CIRCUMFERENCE - ty * tile_size;
        y_min = y_max - tile_size;
    }

    struct map_view
    {
        // View center in Web Mercator meters
        double center_x;
        double center_y;

        // Half the viewport extent in meters (controls zoom)
        // Smaller = more zoomed in
        double half_extent_y;

        map_view()
            : center_x(lon_to_mx(-98.0))  // Center of CONUS
            , center_y(lat_to_my(39.5))
            , half_extent_y(lat_to_my(50.0) - lat_to_my(25.0)) // Show roughly CONUS
        {
        }

        // Viewport bounds in meters (y only; x requires aspect ratio from caller)
        double view_y_min() const { return center_y - half_extent_y; }
        double view_y_max() const { return center_y + half_extent_y; }

        // Pan by fraction of viewport
        void pan(double dx_frac, double dy_frac, double aspect_ratio)
        {
            center_x += dx_frac * half_extent_y * aspect_ratio * 2.0;
            center_y += dy_frac * half_extent_y * 2.0;
            clamp_center();
        }

        // Pan by meters
        void pan_meters(double dx, double dy)
        {
            center_x += dx;
            center_y += dy;
            clamp_center();
        }

        // Zoom by factor around viewport center
        void zoom(double factor, int viewport_height)
        {
            half_extent_y *= factor;
            clamp_extent(viewport_height);
        }

        // Zoom by factor around a point in meters
        void zoom_at(double factor, double px, double py, int viewport_height)
        {
            // Move center toward/away from the zoom point
            center_x = px + (center_x - px) * factor;
            center_y = py + (center_y - py) * factor;
            half_extent_y *= factor;
            clamp_extent(viewport_height);
            clamp_center();
        }

        double zoom_level(int viewport_height) const
        {
            return nasrbrowse::zoom_level(half_extent_y, viewport_height);
        }

        // Set zoom to an exact zoom level
        void zoom_to_level(int viewport_height, int z)
        {
            double world_size = 2.0 * HALF_CIRCUMFERENCE;
            double meters_per_pixel = world_size / (256.0 * std::pow(2.0, z));
            half_extent_y = meters_per_pixel * viewport_height * 0.5;
            clamp_extent(viewport_height);
        }

    private:
        void clamp_center()
        {
            if(center_x < -HALF_CIRCUMFERENCE)
                center_x = -HALF_CIRCUMFERENCE;
            if(center_x > HALF_CIRCUMFERENCE)
                center_x = HALF_CIRCUMFERENCE;
            if(center_y < -HALF_CIRCUMFERENCE)
                center_y = -HALF_CIRCUMFERENCE;
            if(center_y > HALF_CIRCUMFERENCE)
                center_y = HALF_CIRCUMFERENCE;
        }

        static double half_extent_for_zoom(double z, int viewport_height)
        {
            double world_size = 2.0 * HALF_CIRCUMFERENCE;
            double meters_per_pixel = world_size / (256.0 * std::pow(2.0, z));
            return meters_per_pixel * viewport_height * 0.5;
        }

        void clamp_extent(int viewport_height)
        {
            constexpr double min_zoom = 3.0;
            constexpr double max_zoom = 18.0;
            double max_extent = half_extent_for_zoom(min_zoom, viewport_height);
            double min_extent = half_extent_for_zoom(max_zoom, viewport_height);
            if(half_extent_y > max_extent)
                half_extent_y = max_extent;
            if(half_extent_y < min_extent)
                half_extent_y = min_extent;
        }
    };

} // namespace nasrbrowse
