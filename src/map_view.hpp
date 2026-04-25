#pragma once

namespace osect
{
    // Half the Web Mercator circumference in meters (~20037508.34)
    constexpr auto HALF_CIRCUMFERENCE = 20037508.342789244;

    struct pixel_pos
    {
        double x;
        double y;
    };

    struct meter_bounds
    {
        double x_min;
        double y_min;
        double x_max;
        double y_max;
    };

    // Convert longitude (degrees) to Web Mercator x (meters)
    double lon_to_mx(double lon);

    // Convert latitude (degrees) to Web Mercator y (meters)
    double lat_to_my(double lat);

    // Convert Web Mercator x (meters) to longitude (degrees)
    double mx_to_lon(double mx);

    // Convert Web Mercator y (meters) to latitude (degrees)
    double my_to_lat(double my);

    // Convert lon/lat to tile coordinates at given zoom
    void lonlat_to_tile(double lon, double lat, int zoom, int& tx, int& ty);

    // Convert tile coordinates to lon/lat (top-left corner of tile)
    void tile_to_lonlat(int tx, int ty, int zoom, double& lon, double& lat);

    // Compute zoom level from view extent and viewport size
    double zoom_level(double half_extent_y, int viewport_height);

    // Get Web Mercator bounds (meters) for a tile
    meter_bounds tile_bounds_meters(int tx, int ty, int zoom);

    struct map_view
    {
        // View center in Web Mercator meters
        double center_x;
        double center_y;

        // Half the viewport extent in meters (controls zoom)
        double half_extent_y;

        // Viewport dimensions in pixels (set by framebuffer_size_event)
        int viewport_width = 0;
        int viewport_height = 0;

        map_view();

        double aspect_ratio() const;

        // Viewport bounds in Mercator meters
        double view_y_min() const;
        double view_y_max() const;
        double view_x_min() const;
        double view_x_max() const;

        // Pan by fraction of viewport
        void pan(double dx_frac, double dy_frac);

        // Pan by meters
        void pan_meters(double dx, double dy);

        // Zoom by factor around viewport center
        void zoom(double factor);

        // Zoom by factor around a point in meters
        void zoom_at(double factor, double px, double py);

        double zoom_level() const;

        // Set zoom to an exact zoom level
        void zoom_to_level(int z);

        // Convert world lon/lat to pixel coordinates (ImGui display
        // origin, top-left). Handles antimeridian wrap.
        pixel_pos world_to_pixel(double lon, double lat) const;

    private:
        void clamp_center();
        double half_extent_for_zoom(double z) const;
        void clamp_extent();
    };

} // namespace osect
