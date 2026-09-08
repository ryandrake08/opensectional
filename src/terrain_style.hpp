#pragma once

#include <vector>

class ini_config;

namespace osect
{
    enum class terrain_shading
    {
        hillshade,
        hypsometric,
        cruise_relative,
    };

    struct hypsometric_stop
    {
        float elevation_m;
        float r; // 0..1
        float g;
        float b;
    };

    // Shaded-relief rendering parameters: hardcoded defaults, optionally
    // overridden by the [terrain] INI section. Constructing with an
    // out-of-range value throws std::runtime_error naming the key.
    struct terrain_style
    {
        explicit terrain_style(const ini_config& ini);

        terrain_shading mode = terrain_shading::hypsometric;
        float opacity = 0.7F;
        float sun_azimuth_deg = 315.0F;
        float sun_altitude_deg = 45.0F;
        float vertical_exaggeration = 1.0F;

        // Ascending by elevation, at least two stops.
        std::vector<hypsometric_stop> ramp;

        // Relative-to-cruise band edges, feet below cruise altitude,
        // strictly increasing.
        float cruise_warning_ft = 500.0F;
        float cruise_caution_ft = 1000.0F;
        float cruise_clear_ft = 2000.0F;

        // GPU tile LRU capacity.
        int gpu_tile_cache = 128;
    };
} // namespace osect
