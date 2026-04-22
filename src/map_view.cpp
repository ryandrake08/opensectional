#include "map_view.hpp"
#include <cmath>

namespace nasrbrowse
{
    constexpr auto EARTH_RADIUS = 6378137.0;

    double lon_to_mx(double lon)
    {
        return lon * HALF_CIRCUMFERENCE / 180.0;
    }

    double lat_to_my(double lat)
    {
        auto lat_rad = lat * M_PI / 180.0;
        auto y = std::log(std::tan(M_PI / 4.0 + lat_rad / 2.0));
        return y * EARTH_RADIUS;
    }

    double mx_to_lon(double mx)
    {
        return mx * 180.0 / HALF_CIRCUMFERENCE;
    }

    double my_to_lat(double my)
    {
        auto y = my / EARTH_RADIUS;
        auto lat_rad = 2.0 * std::atan(std::exp(y)) - M_PI / 2.0;
        return lat_rad * 180.0 / M_PI;
    }

    void lonlat_to_tile(double lon, double lat, int zoom, int& tx, int& ty)
    {
        auto n = std::pow(2.0, zoom);
        tx = static_cast<int>((lon + 180.0) / 360.0 * n);
        auto lat_rad = lat * M_PI / 180.0;
        ty = static_cast<int>((1.0 - std::log(std::tan(lat_rad) + 1.0 / std::cos(lat_rad)) / M_PI) / 2.0 * n);
    }

    void tile_to_lonlat(int tx, int ty, int zoom, double& lon, double& lat)
    {
        auto n = std::pow(2.0, zoom);
        lon = tx / n * 360.0 - 180.0;
        auto lat_rad = std::atan(std::sinh(M_PI * (1.0 - 2.0 * ty / n)));
        lat = lat_rad * 180.0 / M_PI;
    }

    double zoom_level(double half_extent_y, int viewport_height)
    {
        auto meters_per_pixel = (half_extent_y * 2.0) / viewport_height;
        auto world_size = 2.0 * HALF_CIRCUMFERENCE;
        return std::log2(world_size / (256.0 * meters_per_pixel));
    }

    void tile_bounds_meters(int tx, int ty, int zoom,
                            double& x_min, double& y_min,
                            double& x_max, double& y_max)
    {
        auto n = std::pow(2.0, zoom);
        auto tile_size = 2.0 * HALF_CIRCUMFERENCE / n;
        x_min = -HALF_CIRCUMFERENCE + tx * tile_size;
        x_max = x_min + tile_size;
        // Y is flipped: tile y=0 is at top (north)
        y_max = HALF_CIRCUMFERENCE - ty * tile_size;
        y_min = y_max - tile_size;
    }

    map_view::map_view()
        : center_x(lon_to_mx(-98.0))
        , center_y(lat_to_my(39.5))
        , half_extent_y(lat_to_my(50.0) - lat_to_my(25.0))
    {
    }

    double map_view::aspect_ratio() const
    {
        return static_cast<double>(viewport_width) / viewport_height;
    }

    double map_view::view_y_min() const { return center_y - half_extent_y; }
    double map_view::view_y_max() const { return center_y + half_extent_y; }
    double map_view::view_x_min() const { return center_x - half_extent_y * aspect_ratio(); }
    double map_view::view_x_max() const { return center_x + half_extent_y * aspect_ratio(); }

    void map_view::pan(double dx_frac, double dy_frac)
    {
        center_x += dx_frac * half_extent_y * aspect_ratio() * 2.0;
        center_y += dy_frac * half_extent_y * 2.0;
        clamp_center();
    }

    void map_view::pan_meters(double dx, double dy)
    {
        center_x += dx;
        center_y += dy;
        clamp_center();
    }

    void map_view::zoom(double factor)
    {
        half_extent_y *= factor;
        clamp_extent();
    }

    void map_view::zoom_at(double factor, double px, double py)
    {
        center_x = px + (center_x - px) * factor;
        center_y = py + (center_y - py) * factor;
        half_extent_y *= factor;
        clamp_extent();
        clamp_center();
    }

    double map_view::zoom_level() const
    {
        return nasrbrowse::zoom_level(half_extent_y, viewport_height);
    }

    void map_view::zoom_to_level(int z)
    {
        auto world_size = 2.0 * HALF_CIRCUMFERENCE;
        auto meters_per_pixel = world_size / (256.0 * std::pow(2.0, z));
        half_extent_y = meters_per_pixel * viewport_height * 0.5;
        clamp_extent();
    }

    void map_view::world_to_pixel(double lon, double lat,
                                   float& px, float& py) const
    {
        auto world_x = lon_to_mx(lon);
        constexpr auto W = 2.0 * HALF_CIRCUMFERENCE;
        while(world_x - center_x > HALF_CIRCUMFERENCE) world_x -= W;
        while(center_x - world_x > HALF_CIRCUMFERENCE) world_x += W;
        auto world_y = lat_to_my(lat);

        auto ndc_x = (world_x - center_x) / (2.0 * half_extent_y);
        auto ndc_y = (world_y - center_y) / (2.0 * half_extent_y);

        px = static_cast<float>(ndc_x * viewport_height + viewport_width * 0.5);
        py = static_cast<float>((0.5 - ndc_y) * viewport_height);
    }

    void map_view::clamp_center()
    {
        constexpr auto W = 2.0 * HALF_CIRCUMFERENCE;
        center_x = std::fmod(center_x + HALF_CIRCUMFERENCE, W);
        if(center_x < 0) center_x += W;
        center_x -= HALF_CIRCUMFERENCE;

        if(center_y < -HALF_CIRCUMFERENCE)
            center_y = -HALF_CIRCUMFERENCE;
        if(center_y > HALF_CIRCUMFERENCE)
            center_y = HALF_CIRCUMFERENCE;
    }

    double map_view::half_extent_for_zoom(double z) const
    {
        auto world_size = 2.0 * HALF_CIRCUMFERENCE;
        auto meters_per_pixel = world_size / (256.0 * std::pow(2.0, z));
        return meters_per_pixel * viewport_height * 0.5;
    }

    void map_view::clamp_extent()
    {
        constexpr auto min_zoom = 3.0;
        constexpr auto max_zoom = 18.0;
        auto max_extent = half_extent_for_zoom(min_zoom);
        auto min_extent = half_extent_for_zoom(max_zoom);
        if(half_extent_y > max_extent)
            half_extent_y = max_extent;
        if(half_extent_y < min_extent)
            half_extent_y = min_extent;
    }

} // namespace nasrbrowse
