#include "layer.hpp"
#include "render_context.hpp"
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_projection.hpp>
#include <sdl/copy_pass.hpp>
#include <sdl/render_pass.hpp>
#include <unordered_set>

struct layer::impl
{
    float normalized_viewport_width;
    int viewport_height_pixels;
    glm::mat4 projection_matrix;
    glm::vec4 viewport;

    std::unordered_set<sdl::input_button_t> buttons_down;
    double cursor_last_x;
    double cursor_last_y;

    impl()
        : normalized_viewport_width(1.0F)
        , viewport_height_pixels(0)
        , viewport(0.0F, 0.0F, 0.0F, 0.0F)
        , cursor_last_x(0.0)
        , cursor_last_y(0.0)
    {
    }
};

layer::layer() : pimpl(new impl()), dirty(true)
{
}

layer::~layer() = default;

void layer::framebuffer_size_event(int width, int height)
{
    auto fwidth(static_cast<float>(width));
    auto fheight(static_cast<float>(height));
    this->pimpl->viewport = glm::vec4(0.0F, 0.0F, fwidth, fheight);

    if(height != 0)
    {
        auto normalized_viewport_width(fwidth / fheight);
        this->pimpl->normalized_viewport_width = normalized_viewport_width;
        this->pimpl->viewport_height_pixels = height;
        this->pimpl->projection_matrix = glm::orthoLH_ZO(
            -normalized_viewport_width * 0.5F, normalized_viewport_width * 0.5F,
            -0.5F, 0.5F, -1.0F, 1.0F);
        this->on_resize(normalized_viewport_width, height);
    }
}

void layer::key_event(sdl::input_key_t key, sdl::input_action_t action, sdl::input_mod_t mod)
{
    this->on_key_input(key, action, mod);
}

void layer::button_event(sdl::input_button_t button, sdl::input_action_t action, sdl::input_mod_t mod)
{
    if(action == sdl::input_action::release)
    {
        this->pimpl->buttons_down.erase(button);
    }
    else
    {
        this->pimpl->buttons_down.insert(button);
    }
    this->on_button_input(button, action, mod);
}

void layer::cursor_position_event(double xpos, double ypos)
{
    // Convert from SDL window coordinates (Y=0 at top) to OpenGL coordinates (Y=0 at bottom)
    double ypos_flipped = this->pimpl->viewport.w - ypos;

    glm::mat4 modelview_matrix(1.0F);
    glm::vec3 pixel_pos(xpos, ypos_flipped, 0.0);
    glm::vec3 position(glm::unProject(pixel_pos, modelview_matrix, this->pimpl->projection_matrix, this->pimpl->viewport));

    this->on_cursor_position(position[0], position[1]);
    if(!this->pimpl->buttons_down.empty())
    {
        std::vector<sdl::input_button_t> down(this->pimpl->buttons_down.begin(), this->pimpl->buttons_down.end());
        this->on_drag_input(down, position[0] - this->pimpl->cursor_last_x, position[1] - this->pimpl->cursor_last_y);
    }

    this->pimpl->cursor_last_x = position[0];
    this->pimpl->cursor_last_y = position[1];
}

void layer::scroll_event(double xoffset, double yoffset)
{
    this->on_scroll(xoffset, yoffset);
}

bool layer::update()
{
    dirty = on_update();
    return dirty;
}

void layer::prepare(size_t& size) const
{
    if(dirty)
    {
        on_prepare(size);
    }
}

void layer::copy(sdl::copy_pass& pass)
{
    if(dirty)
    {
        on_copy(pass);
    }
}

void layer::render(sdl::render_pass& pass, nasrbrowse::render_context& ctx) const
{
    ctx.projection_matrix = this->pimpl->projection_matrix;
    ctx.normalized_viewport_width = this->pimpl->normalized_viewport_width;
    on_render(pass, ctx);
}

// Default no-op overrides
void layer::on_resize(float, int) {}
void layer::on_button_input(sdl::input_button_t, sdl::input_action_t, sdl::input_mod_t) {}
void layer::on_cursor_position(double, double) {}
void layer::on_key_input(sdl::input_key_t, sdl::input_action_t, sdl::input_mod_t) {}
void layer::on_drag_input(const std::vector<sdl::input_button_t>&, double, double) {}
void layer::on_scroll(double, double) {}
bool layer::on_update() { return true; }
void layer::on_prepare(size_t&) const {}
void layer::on_copy(sdl::copy_pass&) {}
void layer::on_render(sdl::render_pass&, const nasrbrowse::render_context&) const {}
