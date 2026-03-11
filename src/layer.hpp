#pragma once
#include <cstddef>
#include <glm/glm.hpp>
#include <memory>
#include <sdl/event.hpp>
#include <vector>

namespace sdl
{
    class device;
    class render_pass;
    class copy_pass;
}

namespace nasrbrowse
{
    struct render_context;
}

class layer : public sdl::event_listener
{
    struct impl;
    std::unique_ptr<impl> pimpl;
    bool dirty;

protected:
    virtual void on_resize(float normalized_viewport_width, int viewport_height_pixels);
    virtual void on_key_input(sdl::input_key_t key, sdl::input_action_t action, sdl::input_mod_t mods);
    virtual void on_button_input(sdl::input_button_t button, sdl::input_action_t action, sdl::input_mod_t mods);
    virtual void on_cursor_position(double xpos, double ypos);
    virtual void on_drag_input(const std::vector<sdl::input_button_t>& buttons, double xdelta, double ydelta);
    virtual void on_scroll(double xoffset, double yoffset);
    virtual bool on_update();
    virtual void on_prepare(size_t& size) const;
    virtual void on_copy(sdl::copy_pass& pass);
    virtual void on_render(sdl::render_pass& pass, const nasrbrowse::render_context& ctx) const;

public:
    layer();
    virtual ~layer();

    // sdl::event_listener interface
    void key_event(sdl::input_key_t key, sdl::input_action_t action, sdl::input_mod_t mod) override;
    void button_event(sdl::input_button_t button, sdl::input_action_t action, sdl::input_mod_t mod) override;
    void cursor_position_event(double xpos, double ypos) override;
    void scroll_event(double xoffset, double yoffset) override;
    void framebuffer_size_event(int width, int height) override;

    // render pipeline
    bool update();
    void prepare(size_t& size) const;
    void copy(sdl::copy_pass& pass);
    virtual void render(sdl::render_pass& pass, nasrbrowse::render_context& ctx) const;
};
