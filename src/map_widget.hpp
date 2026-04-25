#pragma once
#include "flight_route.hpp"
#include <memory>
#include <optional>
#include <string>
#include <vector>

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
}

// The main map widget: owns the tile renderer, feature renderer, label
// renderer, pick/info popups, GPU pipelines, and map view (pan/zoom).
// Handles all user input and dispatches render passes.
class map_widget
{
    struct impl;
    std::shared_ptr<impl> pimpl;

public:
    // `tile_path` may be null if no basemap is available.
    // `conf_path` is the chart style INI.
    map_widget(sdl::device& dev, const char* tile_path,
               const char* db_path, const char* conf_path,
               int viewport_width, int viewport_height);
    ~map_widget();

    // Apply per-layer and altitude-band visibility from the UI overlay.
    void set_visibility(const osect::layer_visibility& vis);

    // Recenter the map on a search hit and zoom to fit.
    void focus_on_hit(const osect::search_hit& hit);

    // The ordered list of toggleable feature_types. Used by the UI
    // overlay to build its layer-visibility checkboxes.
    const std::vector<std::unique_ptr<osect::feature_type>>& feature_types() const;

    // Run a full-text search against the NASR database.
    std::vector<osect::search_hit> search(const std::string& query, int limit);

    // Parse `text` as a flight route and activate it. Empty text clears
    // the route. Throws osect::route_parse_error on parse failure —
    // the caller is responsible for surfacing the message.
    void set_route_text(const std::string& text);

    // Clear the active route, if any.
    void clear_route();

    // Currently active route (nullopt if none).
    const std::optional<osect::flight_route>& route() const;

    // Returns true once if the route was mutated internally (e.g. via
    // segment drag-insert) since the last call; used by main() to re-push
    // the route state into the UI overlay.
    bool drain_route_dirty();

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

    // Drain async results and check if rendering is needed.
    bool update();

    // Execute one full render frame: copy (GPU upload) + all render
    // passes (tiles, fills, grid, SDF lines, labels). ImGui rendering
    // is NOT included — caller handles that separately.
    void render_frame(sdl::command_buffer& cmd, sdl::texture& swapchain);
};
