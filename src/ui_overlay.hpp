#pragma once

namespace nasrbrowse
{

    struct layer_visibility
    {
        bool basemap = true;
        bool airports = true;
        bool runways = true;
        bool navaids = true;
        bool fixes = true;
        bool airways = true;
        bool airspace = true;
        bool sua = true;
        bool obstacles = true;
    };

    class ui_overlay
    {
        layer_visibility vis;

    public:
        ui_overlay();

        // Draw FPS display and layer checkboxes. Returns true if visibility changed.
        bool draw(float last_render_ms);

        const layer_visibility& visibility() const;
    };

} // namespace nasrbrowse
