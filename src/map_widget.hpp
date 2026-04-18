#pragma once
#include <memory>
#include <sdl/event.hpp>
#include <string>
#include <vector>

namespace sdl
{
    class command_buffer;
    class device;
    class texture;
}

namespace nasrbrowse
{
    struct layer_visibility;
    struct search_hit;
    class feature_type;
}

// The main map widget: owns the tile renderer, feature renderer, label
// renderer, pick/info popups, GPU pipelines, and map view (pan/zoom).
// Handles all user input and dispatches render passes.
class map_widget : public sdl::event_listener
{
    struct impl;
    std::unique_ptr<impl> pimpl;

public:
    // `tile_path` may be null if no basemap is available.
    // `conf_path` is the chart style INI.
    map_widget(sdl::device& dev, const char* tile_path,
               const char* db_path, const char* conf_path);
    ~map_widget() override;

    // Apply per-layer and altitude-band visibility from the UI overlay.
    void set_visibility(const nasrbrowse::layer_visibility& vis);

    // Recenter the map on a search hit and zoom to fit.
    void focus_on_hit(const nasrbrowse::search_hit& hit);

    // The ordered list of toggleable feature_types. Used by the UI
    // overlay to build its layer-visibility checkboxes.
    const std::vector<std::unique_ptr<nasrbrowse::feature_type>>& feature_types() const;

    // Run a full-text search against the NASR database.
    std::vector<nasrbrowse::search_hit> search(const std::string& query, int limit);

    // Tell the map whether ImGui is consuming the mouse this frame
    // (suppresses pick on click when true).
    void set_imgui_wants_mouse(bool wants);

    // Current map zoom level (for UI display).
    double zoom_level() const;

    // Draw map-owned ImGui overlays (pick-selector and info popups).
    // Call during an ImGui frame (between new_frame and end_frame).
    // Returns true if another frame is needed for popup warmup.
    bool draw_imgui();

    // sdl::event_listener interface
    void key_event(sdl::input_key_t key, sdl::input_action_t action, sdl::input_mod_t mod) override;
    void button_event(sdl::input_button_t button, sdl::input_action_t action, sdl::input_mod_t mod) override;
    void cursor_position_event(double xpos, double ypos) override;
    void scroll_event(double xoffset, double yoffset) override;
    void framebuffer_size_event(int width, int height) override;

    // Drain async results and check if rendering is needed.
    bool update();

    // Execute one full render frame: copy (GPU upload) + all render
    // passes (tiles, fills, grid, SDF lines, labels). ImGui rendering
    // is NOT included — caller handles that separately.
    void render_frame(sdl::command_buffer& cmd, sdl::texture& swapchain);
};
