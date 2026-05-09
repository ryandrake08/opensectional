#pragma once

#include "pick_result.hpp"
#include <memory>
#include <optional>
#include <vector>

namespace osect
{
    class feature_type;
    class flight_route;
    struct map_view;

    // Owns and draws the three transient popups that hover over the map:
    // the pick selector (multi-feature list at click point), the info popup
    // (single-feature details), and the route info popup (active flight
    // route legs and Delete button).
    //
    // The manager owns popup *state* only — open/close flags, anchor
    // coordinates, session/window IDs, warmup counters. Side effects on
    // other subsystems (feature_renderer selection, route lifetime,
    // re-render flags) are reported via the actions struct returned from
    // draw() and applied by the caller.
    class popup_manager
    {
        struct impl;
        std::unique_ptr<impl> pimpl;

    public:
        popup_manager();
        ~popup_manager();

        popup_manager(const popup_manager&) = delete;
        popup_manager& operator=(const popup_manager&) = delete;

        void open_pick(std::vector<feature> features, double click_lon, double click_lat);
        void close_pick();
        // Open the info popup for `f`. When `f` holds a route_pick,
        // the body renders the legs table + Delete button instead
        // of the kv table.
        void open_info(const feature& f, double anchor_lon, double anchor_lat);
        void close_info();

        bool pick_open() const;
        bool info_open() const;
        // True iff info_open() and the currently displayed feature
        // is a route_pick. Used by callers that need to distinguish
        // "route popup" from "feature popup".
        bool info_open_for_route() const;

        struct pick_selection
        {
            feature picked;
            double click_lon;
            double click_lat;
        };

        // User actions surfaced for the current frame. Each *_dismissed
        // and *_delete flag means the manager has already closed the
        // popup; the caller's job is the consequent side effects.
        struct actions
        {
            bool needs_more_frames = false;
            std::optional<pick_selection> pick_selected;
            bool pick_dismissed = false;
            bool info_dismissed = false;
            // The user clicked Delete on a route info popup. Set
            // only when the info popup was showing a route_pick;
            // the caller drains and removes the route.
            bool route_delete = false;
        };

        // Draw all open popups. `routes` is needed to render the
        // legs table when the info popup is showing a route_pick,
        // and to auto-close the popup if the route disappeared.
        actions draw(const map_view& view, const std::vector<std::unique_ptr<feature_type>>& feature_types,
                     const std::vector<flight_route>& routes);
    };

    // Draw rubber-band lines from the dragged route waypoint(s) to the
    // current ImGui mouse cursor — visual affordance for an in-progress
    // route drag. `is_segment_drag` selects between segment-drag (lines
    // from waypoints `index` and `index+1`) and waypoint-drag (lines from
    // the neighbors of `index`, when they exist).
    void draw_route_drag_rubber_band(const map_view& view, const flight_route& route, bool is_segment_drag,
                                     std::size_t index);

} // namespace osect
