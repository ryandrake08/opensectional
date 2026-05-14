#pragma once
#include "ephemeral_source.hpp"
#include "flight_route.hpp"
#include <memory>
#include <optional>
#include <string>
#include <vector>

class ini_config;

namespace sdl
{
    class command_buffer;
    class device;
    struct event_listener;
    class texture;
}

namespace osect
{
    struct layer_visibility;
    struct search_hit;
    class feature_type;

    // The main map widget: owns the tile renderer, feature renderer, label
    // renderer, pick/info popups, GPU pipelines, and map view (pan/zoom).
    // Handles all user input and dispatches render passes.
    class map_widget
    {
        struct impl;
        std::shared_ptr<impl> pimpl;

    public:
        // `tile_path` may be null if no basemap is available.
        // `ini` carries chart-style overrides; pass an empty ini_config{}
        // for code defaults only.
        // Feature build / pick paths open their own read-only
        // connections to the platform-default ephemeral.db, so
        // ephemeral data flows through SQLite rather than through
        // a passed-in facade.
        map_widget(sdl::device& dev, const char* tile_path, const char* db_path, const ini_config& ini,
                   int viewport_width, int viewport_height);
        ~map_widget();

        // Apply per-layer and altitude-band visibility from the UI overlay.
        void set_visibility(const layer_visibility& vis);

        // Recenter the map on a search hit and zoom to fit.
        void focus_on_hit(const search_hit& hit);

        // The ordered list of toggleable feature_types. Used by the UI
        // overlay to build its layer-visibility checkboxes.
        const std::vector<std::unique_ptr<feature_type>>& feature_types() const;

        // Run a full-text search against the NASR database.
        std::vector<search_hit> search(const std::string& query, int limit);

        // Notify map_widget that a new route exists in user.db.
        // Caller has already persisted; this just flags a rebuild
        // so the next feature build picks it up. Pure append — does
        // not change the active or selected route, and does not
        // move the view (see fit_view_to_route / set_active_route /
        // select_route for those).
        void add_route(route_id id);

        // Center and zoom the view to fit the bounding box of the
        // named route. No effect on active/selected. No-op if the
        // route doesn't exist or fails to parse.
        void fit_view_to_route(route_id id);

        // Make `id` the selected route — opens the route info popup
        // anchored at the route's centroid and renders that route
        // in white + halos. nullopt closes the popup and
        // un-highlights. Independent of the active route.
        void select_route(std::optional<route_id> id);

        // Notify map_widget that a route's text has been replaced
        // in user.db. Caller has already persisted; this just flags
        // a rebuild.
        void replace_route(route_id id);

        // Forget the route — clears active/selected if either
        // referenced this id and flags a rebuild. Caller is
        // responsible for the user.db delete.
        void remove_route(route_id id);

        // Make `id` the active route. nullopt clears active. The
        // active route is the panel/drag target. Independent of the
        // selected (highlighted) route.
        void set_active_route(std::optional<route_id> id);

        // The active route, or nullopt if none.
        std::optional<route_id> active_route() const;

        // If a drag finished since the last call, returns the
        // mutated flight_route (already committed to user.db). The
        // caller pushes its new text into the UI overlay. nullopt
        // otherwise.
        std::optional<flight_route> drain_route_drag_result();

        // If the user clicked Delete on the route info popup since
        // the last call, return the route id that was selected at
        // the time. The route is NOT removed from user.db by
        // map_widget — caller owns persistence so it can close the
        // corresponding panel tab in the same operation. nullopt
        // otherwise.
        std::optional<route_id> drain_route_delete_request();

        // If the user clicked a non-active route's leg or waypoint
        // since the last call, return that route's id. The caller
        // switches the panel tab to the corresponding tab and
        // updates active/selection accordingly. nullopt otherwise.
        std::optional<route_id> drain_route_activate_request();

        // Tell the map whether ImGui is consuming the mouse this frame
        // (suppresses pick on click when true).
        void set_imgui_wants_mouse(bool wants);

        // Tell the map whether ImGui is consuming the keyboard this frame
        // (suppresses WASD / R / F pan+zoom when a text box has focus).
        void set_imgui_wants_keyboard(bool wants);

        // Current map zoom level (for UI display).
        double zoom_level() const;

        // Draw map-owned ImGui overlays (pick-selector and info popups).
        // Call during an ImGui frame (between new_frame and end_frame).
        // Returns true if another frame is needed for popup warmup.
        bool draw_imgui();

        // The SDL event listener for this widget. Register with
        // sdl::event_manager::add_listener. The listener owns its own
        // shared lifetime, so it stays alive for as long as the event
        // manager keeps a reference even if the map_widget is destroyed.
        std::shared_ptr<sdl::event_listener> event_listener();

        // Notify that an ephemeral data source has new data on disk.
        // Invalidates any cached projection that depends on it (today
        // just the feature build) and flags a redraw. Called from
        // the main thread by the event handler installed on the
        // ephemeral_refresh_event_type SDL event.
        void on_ephemeral_refresh(ephemeral_source source);

        // Per-frame sync. Called once at the end of each iteration
        // of the main loop after all state-mutating code (input
        // handlers, popup actions). Drains any async tile / feature
        // build results that arrived since the last call, then
        // submits new tile and feature build requests so the next
        // frame can reflect the latest state. Returns true if the
        // frame needs to be rendered (new geometry uploaded, labels
        // reprojected, etc.).
        bool update();

        // Execute one full render frame: copy (GPU upload) + all render
        // passes (tiles, fills, grid, SDF lines, labels). ImGui rendering
        // is NOT included — caller handles that separately.
        void render_frame(sdl::command_buffer& cmd, sdl::texture& swapchain);
    };

}
