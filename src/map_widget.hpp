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
    class ephemeral_data;

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
        // `eph` is the app's ephemeral data facade; the feature
        // build/pick paths read TFRs (and any future ephemeral
        // sources) through it in place of SQL-backed queries.
        map_widget(sdl::device& dev, const char* tile_path, const char* db_path, const ini_config& ini,
                   ephemeral_data& eph, int viewport_width, int viewport_height);
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

        // Append a parsed route. Pure append: does not change the
        // active or selected indexes, and does not move the view.
        // Caller decides which of those side effects to apply (see
        // fit_view_to_route, set_active_route, select_route).
        void add_route(flight_route route);

        // Center and zoom the view to fit the bounding box of the
        // route at `index`. No effect on active/selected. Asserts
        // on out-of-range.
        void fit_view_to_route(std::size_t index);

        // Make `index` the selected route — opens the route info
        // popup anchored at the route's centroid and renders that
        // route in white + halos. nullopt closes the popup and
        // un-highlights. Independent of the active route. Asserts
        // on out-of-range.
        void select_route(std::optional<std::size_t> index);

        // Replace the route at `index` with a freshly-planned one.
        // Used when re-submitting a panel that already has a route
        // so the route's position in the routes vector (and any
        // tab-id ↔ index mapping the caller maintains) doesn't shift.
        void replace_route(std::size_t index, flight_route route);

        // Remove the route at `index`. Adjusts active and selected
        // indexes so each still points to a valid route or becomes
        // nullopt. Asserts on out-of-range index.
        void remove_route(std::size_t index);

        // Remove every route and clear the active/selected indexes.
        void clear_routes();

        // Make `index` the active route. nullopt clears active. The
        // active route is the panel/drag target. Independent of the
        // selected (highlighted) route. Asserts on out-of-range.
        void set_active_route(std::optional<std::size_t> index);

        // The full route list (read-only).
        const std::vector<flight_route>& routes() const;

        // Index of the active route, or nullopt if none.
        std::optional<std::size_t> active_route_index() const;

        // Returns true once if a route was mutated internally (e.g. via
        // segment drag-insert) since the last call; used by main() to re-push
        // the route state into the UI overlay.
        bool drain_route_dirty();

        // If the user clicked Delete on the route info popup since
        // the last call, return the route index that was selected at
        // the time. The route is NOT removed by map_widget — caller
        // owns the lifecycle (so the corresponding panel tab can be
        // closed in the same operation). Returns nullopt otherwise.
        std::optional<std::size_t> drain_route_delete_request();

        // If the user clicked a non-active route's leg or waypoint
        // since the last call, return that route's index. The
        // caller switches the panel tab to the corresponding tab
        // and updates active/selection accordingly. Returns nullopt
        // otherwise.
        std::optional<std::size_t> drain_route_activate_request();

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
