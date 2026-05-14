#pragma once

#include "altitude_filter.hpp"
#include "chart_type.hpp"
#include "data_source.hpp"
#include "flight_route.hpp"
#include "nasr_database.hpp" // for search_hit (POD struct, not the class)
#include <array>
#include <cstdint>
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
        chart_type chart = chart_type::sectional;

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
        // Set for one frame whenever the search box text changes —
        // the value is the new query (possibly empty if the user
        // erased it). Caller runs the FTS query and feeds results
        // back via set_search_results(). Unset when the box hasn't
        // changed since the previous frame.
        std::optional<std::string> search_query;
        // Copy of the hit that the user just selected (click or Enter).
        // Set for exactly one frame.
        std::optional<int> selected_hit_index;

        // Submission requested by a route panel tab this frame.
        // `text` is empty for the Clear button (the tab stays open;
        // the caller drops the route). Non-empty text is handed to
        // route_submitter; the caller feeds state back via
        // set_route_state() / set_route_error() addressed by tab_id.
        struct route_submit_request
        {
            std::uint64_t tab_id;
            std::string text;
            double max_leg_nm;
            bool use_airways;
        };
        std::optional<route_submit_request> route_submit;

        // The id of a tab the user just closed via its X button.
        // ui_overlay has already removed the panel; the caller is
        // responsible for removing the tab's route from map_widget
        // (if it had one) and clearing its tab_id ↔ route_index
        // mapping.
        std::optional<std::uint64_t> tab_closed;

        // The active tab changed this frame. The caller propagates
        // the new tab's route_index to map_widget::set_active_route.
        std::optional<std::uint64_t> active_tab_changed;
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

        // Programmatically create a new panel pre-populated with a
        // planned route, returning its tab_id. Used at startup to
        // restore tabs for routes loaded from user.db. If the only
        // existing panel is the pristine starter (no route, no
        // text), it's populated in place so loading N saved routes
        // produces exactly N tabs.
        std::uint64_t add_route_tab(const flight_route& route);

        // Mark the panel for `tab_id` as holding a planned route.
        // Snaps the input buffer to the canonical shorthand (so any
        // entry/exit auto-corrections appear) and clears any error.
        void set_route_state(std::uint64_t tab_id, const flight_route& route);

        // Mark the panel for `tab_id` as having no planned route.
        // `error` is shown in red beneath the input when non-empty —
        // typically the message from a failed parse.
        void clear_route_state(std::uint64_t tab_id, const std::string& error = "");

        // Toggle the async-planning indicator on the panel for
        // `tab_id`. Caller sets this true when a sigil-bearing route
        // is being expanded on a background thread, and back to
        // false when the plan completes. The spinner stays anchored
        // to its tab even if the user switches to a different one.
        void set_route_planning(std::uint64_t tab_id, bool pending);

        // Programmatically close the panel for `tab_id` (e.g. in
        // response to the route-info popup's Delete button). The
        // tab_closed event is NOT emitted, since the caller already
        // knows it asked for the close. If the tab doesn't exist,
        // no-op.
        void close_tab(std::uint64_t tab_id);

        // Programmatically switch the active panel to `tab_id`
        // (e.g. when the user clicked a non-active route on the
        // map). The active_tab_changed event is NOT emitted, since
        // the caller already knows the new active and is expected
        // to update map state inline. If the tab doesn't exist,
        // no-op.
        void set_active_tab(std::uint64_t tab_id);

        // True if a panel with this id currently exists. Used by the
        // caller to drop async route-plan results addressed at a tab
        // that has since been closed.
        bool has_tab(std::uint64_t tab_id) const;

        // Seed the per-tab planner-knob defaults applied to newly
        // created panels. Typically called once at startup with
        // values loaded from ini. Existing panels keep whatever
        // values their user already set.
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
