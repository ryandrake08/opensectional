#pragma once

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "altitude_filter.hpp"
#include "nasr_database.hpp"  // for search_hit (POD struct, not the class)

namespace nasrbrowse
{
    class feature_type;

    // Layer identifiers. The SDF layers are ordered back-to-front for rendering.
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
        layer_obstacles,
        layer_rco,
        layer_awos,
        layer_fixes,
        layer_navaids,
        layer_airports,
        layer_sdf_count,

        // Non-SDF layers
        layer_basemap = layer_sdf_count,
        layer_count
    };

    struct layer_visibility
    {
        std::array<bool, layer_count> visible;
        altitude_filter altitude;

        layer_visibility()
        {
            visible.fill(true);
        }

        bool operator[](int id) const { return visible[id]; }
        bool& operator[](int id) { return visible[id]; }
    };

    struct ui_overlay_result
    {
        bool visibility_changed = false;
        // Current query text in the search box. Caller runs the actual
        // FTS query and feeds results back via set_search_results().
        std::string search_query;
        // Copy of the hit that the user just selected (click or Enter).
        // Set for exactly one frame.
        std::optional<int> selected_hit_index;
    };

    class ui_overlay
    {
        struct impl;
        std::unique_ptr<impl> pimpl;

    public:
        ui_overlay();
        ~ui_overlay();

        // Current results to display beneath the search box. Caller sets
        // these after running nasr_database::search(query). Passing an
        // empty vector clears the dropdown.
        void set_search_results(std::vector<search_hit> hits);
        // Access the currently displayed results (caller uses the selected
        // index from ui_overlay_result to retrieve the chosen hit).
        const std::vector<search_hit>& search_results() const;

        // Draw FPS display, zoom level, layer checkboxes, and the search box.
        // `feature_types` supplies the labels+ids for the feature-layer
        // checkboxes (the basemap row is always prepended).
        ui_overlay_result draw(
            float last_render_ms, double zoom_level,
            const std::vector<std::unique_ptr<feature_type>>& feature_types);

        // Access the list of visible/invisible layers
        const layer_visibility& visibility() const;
    };

} // namespace nasrbrowse
