#pragma once
#include <memory>
#include <sdl/event.hpp>
#include <vector>

namespace sdl
{
    class device;
    class font;
    class render_pass;
    class copy_pass;
    class sampler;
    class text_engine;
}

namespace nasrbrowse
{
    struct layer_visibility;
    struct search_hit;
    struct render_context;
    class chart_style;
    class nasr_database;
    class feature_type;
}

// The main map widget: owns the tile renderer, feature renderer, label
// renderer, pick/info popups, and map view (pan/zoom). Handles all
// user input and dispatches render passes.
class map_widget : public sdl::event_listener
{
    struct impl;
    std::unique_ptr<impl> pimpl;

public:
    // `db` is borrowed for picks and search navigation (owned by main).
    // `tile_path` may be null if no basemap is available.
    map_widget(sdl::device& dev, const char* tile_path, const char* db_path,
              nasrbrowse::nasr_database& db,
              const nasrbrowse::chart_style& cs,
              sdl::text_engine& text_engine, sdl::font& font,
              sdl::font& outline_font,
              const sdl::sampler& text_sampler);
    ~map_widget() override;

    // Apply per-layer and altitude-band visibility from the UI overlay.
    void set_visibility(const nasrbrowse::layer_visibility& vis);

    // Recenter the map on a search hit and zoom to fit.
    void focus_on_hit(const nasrbrowse::search_hit& hit);

    // The ordered list of toggleable feature_types. Used by the UI
    // overlay to build its layer-visibility checkboxes.
    const std::vector<std::unique_ptr<nasrbrowse::feature_type>>& feature_types() const;

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

    // Render pipeline: drain async results, upload GPU data, draw.
    bool update();
    void copy(sdl::copy_pass& pass);
    void render(sdl::render_pass& pass, nasrbrowse::render_context& ctx) const;
};
