#pragma once
#include "layer.hpp"
#include <memory>
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
    class chart_style;
    class nasr_database;
    class feature_type;
}

class layer_map : public layer
{
    struct impl;
    std::unique_ptr<impl> pimpl;

public:
    layer_map(sdl::device& dev, const char* tile_path, const char* db_path,
              nasrbrowse::nasr_database& db,
              const nasrbrowse::chart_style& cs,
              sdl::text_engine& text_engine, sdl::font& font,
              sdl::font& outline_font,
              const sdl::sampler& text_sampler);
    ~layer_map() override;

    void set_visibility(const nasrbrowse::layer_visibility& vis);

    // Recenter the map on a search hit and zoom to fit.
    void focus_on_hit(const nasrbrowse::search_hit& hit);

    // The ordered list of toggleable feature_types. Used by the UI
    // overlay to build its layer-visibility checkboxes.
    const std::vector<std::unique_ptr<nasrbrowse::feature_type>>& feature_types() const;
    void set_imgui_wants_mouse(bool wants);
    double zoom_level() const;

    // Draw map-owned ImGui overlays (the pick-selector popup). Call during
    // an ImGui frame (between new_frame and end_frame). Returns true if
    // another frame is needed (e.g. warming up a freshly opened popup
    // whose auto-resized window ImGui hides for its first measurement frame).
    bool draw_imgui();

    void on_button_input(sdl::input_button_t button, sdl::input_action_t action, sdl::input_mod_t mods) override;
    void on_cursor_position(double xpos, double ypos) override;
    void on_key_input(sdl::input_key_t key, sdl::input_action_t action, sdl::input_mod_t mods) override;
    void on_drag_input(const std::vector<sdl::input_button_t>& buttons, double xdelta, double ydelta) override;
    void on_scroll(double xoffset, double yoffset) override;
    void on_resize(float normalized_viewport_width, int viewport_height_pixels) override;
    bool on_update() override;
    void on_copy(sdl::copy_pass& pass) override;
    void on_render(sdl::render_pass& pass, const nasrbrowse::render_context& ctx) const override;
};
