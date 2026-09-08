#pragma once

#include "tile_key.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace osect
{
    struct elevation_address
    {
        tile_key key;
        double pixel_x;
        double pixel_y;
    };

    // Maps geographic coordinates to a terrain tile and its skirt-aware pixel.
    inline elevation_address address_elevation(double lat, double lon, int zoom, int tile_pixels, int skirt_pixels)
    {
        if(!std::isfinite(lat) || !std::isfinite(lon) || zoom < 0 || zoom > 30 || tile_pixels <= 0 || skirt_pixels < 0)
        {
            throw std::invalid_argument("invalid elevation tile geometry");
        }

        constexpr double max_latitude = 85.0511287798066;
        constexpr double pi = 3.14159265358979323846;
        const int tiles_per_axis = 1 << zoom;
        const double world_pixels = static_cast<double>(tiles_per_axis) * tile_pixels;

        lon = std::fmod(lon + 180.0, 360.0);
        if(lon < 0.0)
        {
            lon += 360.0;
        }
        lat = std::clamp(lat, -max_latitude, max_latitude);

        double pixel_x = lon / 360.0 * world_pixels;
        const double latitude_radians = lat * pi / 180.0;
        double pixel_y = (1.0 - std::asinh(std::tan(latitude_radians)) / pi) * 0.5 * world_pixels;
        pixel_x = std::min(pixel_x, std::nextafter(world_pixels, 0.0));
        pixel_y = std::clamp(pixel_y, 0.0, std::nextafter(world_pixels, 0.0));

        const int tile_x = static_cast<int>(pixel_x / tile_pixels);
        const int tile_y = static_cast<int>(pixel_y / tile_pixels);
        const double local_x = pixel_x - tile_x * tile_pixels;
        const double local_y = pixel_y - tile_y * tile_pixels;

        return {{zoom, tile_x, tile_y}, skirt_pixels + local_x - 0.5, skirt_pixels + local_y - 0.5};
    }
} // namespace osect
