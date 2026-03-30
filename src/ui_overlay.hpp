#pragma once

#include <array>

namespace nasrbrowse
{

    // Layer identifiers. The SDF layers (artcc through balloonports) are ordered
    // back-to-front for rendering. Basemap and obstacles are non-SDF layers.
    enum layer
    {
        // SDF polyline layers (render order: first = back, last = front)
        layer_adiz,
        layer_artcc,
        layer_sua,
        layer_pja,
        layer_maa,
        layer_airspace,
        layer_runways,
        layer_airways,
        layer_mtrs,
        layer_fixes,
        layer_navaids,
        layer_airports,
        layer_heliports,
        layer_seaplane,
        layer_ultralight,
        layer_gliderports,
        layer_balloonports,
        layer_sdf_count,

        // Non-SDF layers
        layer_basemap = layer_sdf_count,
        layer_obstacles,
        layer_count
    };

    struct layer_info
    {
        const char* label;
        int id;
    };

    // UI display order for checkboxes
    inline constexpr layer_info layer_entries[] = {
        {"Basemap",      layer_basemap},
        {"Airports",     layer_airports},
        {"Heliports",    layer_heliports},
        {"Seaplane",     layer_seaplane},
        {"Ultralight",   layer_ultralight},
        {"Gliderports",  layer_gliderports},
        {"Balloonports", layer_balloonports},
        {"Runways",      layer_runways},
        {"Navaids",      layer_navaids},
        {"Fixes",        layer_fixes},
        {"Airways",      layer_airways},
        {"MTRs",         layer_mtrs},
        {"PJA",          layer_pja},
        {"MAA",          layer_maa},
        {"Airspace",     layer_airspace},
        {"SUA",          layer_sua},
        {"ADIZ",         layer_adiz},
        {"ARTCC",        layer_artcc},
        {"Obstacles",    layer_obstacles},
    };

    struct layer_visibility
    {
        std::array<bool, layer_count> visible;

        layer_visibility()
        {
            visible.fill(true);
        }

        bool operator[](int id) const { return visible[id]; }
        bool& operator[](int id) { return visible[id]; }
    };

    class ui_overlay
    {
        layer_visibility vis;

    public:
        ui_overlay();

        // Draw FPS display, zoom level, and layer checkboxes. Returns true if visibility changed.
        bool draw(float last_render_ms, double zoom_level);

        const layer_visibility& visibility() const;
    };

} // namespace nasrbrowse
