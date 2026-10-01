#pragma once

#include <glm/glm.hpp>

namespace osect
{
    // ============================================================================
    // Uniform Buffer Structures
    // ============================================================================

    // Universal uniform buffer structure (superset of all shader uniforms)
    // All shaders accept this structure but only use the fields they need.
    // This eliminates the need for multiple uniform structures and simplifies
    // shader switching without changing uniform layout.
    // Total size: 416 bytes (naturally aligned)
    struct uniform_buffer
    {
        // Core transformation matrices (used by ALL shaders)
        glm::mat4 projection_matrix; // 64 bytes, offset 0   - Camera projection
        glm::mat4 view_matrix;       // 64 bytes, offset 64  - Camera view transform
        glm::mat4 model_matrix;      // 64 bytes, offset 128 - Model-to-world transform
        glm::mat4 texture_matrix;    // 64 bytes, offset 192 - Texture coordinate transform
        glm::mat4 color_matrix;      // 64 bytes, offset 256 - Color transformation

        // Text outline parameters (used by outline_text shader)
        glm::vec4 outline_color; // 16 bytes, offset 320 - RGBA outline color
        glm::ivec2 texture_size; // 8 bytes, offset 336  - Texture atlas dimensions

        // Y-cut parameters (used by ALL shaders)
        float y_min; // 4 bytes, offset 344 - Minimum Y for clip test
        float y_max; // 4 bytes, offset 348 - Maximum Y for clip test

        // Terrain hillshade parameters (used by terrain shader)
        float sun_azimuth;           // 4 bytes, offset 352 - radians, clockwise from north
        float sun_altitude;          // 4 bytes, offset 356 - radians above the horizon
        float vertical_exaggeration; // 4 bytes, offset 360 - slope multiplier
        float terrain_opacity;       // 4 bytes, offset 364 - layer alpha, 0..1
        float terrain_texel_m;       // 4 bytes, offset 368 - Web Mercator metres per height texel
        int terrain_mode;            // 4 bytes, offset 372 - 0 hillshade, 1 hypsometric, 2 relative-to-cruise
        float hypso_min_m;           // 4 bytes, offset 376 - elevation mapped to the ramp start
        float hypso_max_m;           // 4 bytes, offset 380 - elevation mapped to the ramp end
        float taws_cruise_m;         // 4 bytes, offset 384 - cruise altitude, relative-to-cruise mode
        float taws_warning_m;        // 4 bytes, offset 388 - metres below cruise for the warning band
        float taws_caution_m;        // 4 bytes, offset 392 - metres below cruise for the caution band
        float taws_clear_m;          // 4 bytes, offset 396 - metres below cruise where terrain stops drawing

        // Water-mask parameters (used by terrain shader)
        int water_mode; // 4 bytes, offset 400 - 0 no water mask, 1 flat-tint water fragments
        float water_r;  // 4 bytes, offset 404 - flat water tint, 0..1
        float water_g;  // 4 bytes, offset 408
        float water_b;  // 4 bytes, offset 412

        // Default constructor: Initialize all fields to safe defaults
        uniform_buffer()
            : projection_matrix(1.0F), // Identity matrix
              view_matrix(1.0F),       // Identity matrix
              model_matrix(1.0F),      // Identity matrix
              texture_matrix(1.0F),    // Identity matrix
              color_matrix(1.0F),      // Identity matrix
              outline_color(0.0F),     // Transparent black
              texture_size(0),         // Zero size
              y_min(-1e9F),            // Default min far below (effectively disabled)
              y_max(1e9F),             // Default max far above (effectively disabled)
              sun_azimuth(0.0F),
              sun_altitude(0.0F),
              vertical_exaggeration(1.0F),
              terrain_opacity(1.0F),
              terrain_texel_m(0.0F),
              terrain_mode(0),
              hypso_min_m(0.0F),
              hypso_max_m(4500.0F),
              taws_cruise_m(0.0F),
              taws_warning_m(152.0F),
              taws_caution_m(305.0F),
              taws_clear_m(610.0F),
              water_mode(0),
              water_r(0.66F),
              water_g(0.78F),
              water_b(0.88F)
        {
        }
    };
} // namespace osect
