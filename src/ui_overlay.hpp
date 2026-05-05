#pragma once

#include "altitude_filter.hpp"
#include "data_source.hpp"
#include "flight_route.hpp"
#include "nasr_database.hpp" // for search_hit (POD struct, not the class)
#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace osect
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
        layer_route_halo,
        layer_fixes,
        layer_navaids,
        layer_airports,
        layer_tfr,
        layer_route,
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

        bool operator[](int id) const
        {
            return visible.at(id);
        }
        bool& operator[](int id)
        {
            return visible.at(id);
        }
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

        // Route text submitted this frame (Enter or "Set" button).
        // Caller parses it and feeds state back via set_route_state().
        std::optional<std::string> submit_route_text;
        // True for one frame when the user clicks "Clear".
        bool clear_route = false;

        // Current route-planner knobs. Read every frame; the caller
        // funnels these into route_planner::options on submission.
        double route_max_leg_nm = 80.0;
        bool route_use_airways = false;
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

        // Update the route panel's displayed state after the caller parses
        // a submitted route text successfully.
        void set_route_state(const flight_route& route);

        // Clear the route panel (no active route). `error` is shown in red
        // if non-empty — typically the message from a failed parse.
        void clear_route_state(const std::string& error = "");

        // Toggle an async-planning indicator shown beneath the route
        // input. Caller sets this to true when a sigil-bearing route
        // is being expanded on a background thread, and back to
        // false when the plan completes.
        void set_route_planning(bool pending);

        // Seed the planner-knob state shown in the route panel.
        // Typically called once at startup with values loaded from
        // ini. The current values are echoed back through
        // ui_overlay_result::route_max_leg_nm / route_use_airways
        // every frame so the caller doesn't need to mirror state.
        void set_route_planner_defaults(double max_leg_nm, bool use_airways);

        // Seed the data-status panel with the per-source freshness
        // records. Typically called once at startup; ephemeral sources
        void set_data_sources(std::vector<data_source> sources);

        // Draw FPS display, layer checkboxes, search box, route panel,
        // and (when populated) the data-status panel. `feature_types`
        // supplies the labels+ids for the feature-layer checkboxes
        // (the basemap row is always prepended).
        ui_overlay_result draw(float last_render_ms, const std::vector<std::unique_ptr<feature_type>>& feature_types);

        // Access the list of visible/invisible layers
        const layer_visibility& visibility() const;
    };

} // namespace osect
