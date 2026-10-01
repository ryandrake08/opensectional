#pragma once

#include "terrain_profile.hpp"
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

    // Shaded-relief rendering parameters and route-against-terrain
    // settings: hardcoded defaults, optionally overridden by the INI.
    // [terrain] holds the relief and cache keys; [route_terrain] holds
    // the cruise_relative band edges and the profile margins.
    // Constructing with an out-of-range value throws std::runtime_error
    // naming the key.
    struct terrain_style
    {
        explicit terrain_style(const ini_config& ini);

        terrain_shading mode = terrain_shading::hypsometric;
        float opacity = 0.7F;
        float sun_azimuth_deg = 315.0F;
        float sun_altitude_deg = 45.0F;
        float vertical_exaggeration = 1.0F;

        // Flat tint for water-mask fragments (0..1). Default a pale
        // sectional blue; overridden by [terrain] water_color = #RRGGBB.
        float water_r = 168.0F / 255.0F;
        float water_g = 200.0F / 255.0F;
        float water_b = 224.0F / 255.0F;

        // Ascending by elevation, at least two stops.
        std::vector<hypsometric_stop> ramp;

        // Relative-to-cruise band edges, feet below cruise altitude,
        // strictly increasing.
        float cruise_warning_ft = 500.0F;
        float cruise_caution_ft = 1000.0F;
        float cruise_clear_ft = 2000.0F;

        // Route terrain-profile corridor width and required clearance.
        terrain_profile_margins margins;

        // GPU tile LRU capacity.
        int gpu_tile_cache = 128;

        // Decoded-tile LRU capacity for CPU elevation queries (route
        // profiles and point elevation).
        int cpu_tile_cache = 128;
    };
} // namespace osect
