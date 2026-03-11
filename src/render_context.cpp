#include "render_context.hpp"
#include <sdl/device.hpp>

namespace nasrbrowse
{
    render_context::render_context(const sdl::device& dev)
        : current_pass(render_pass_id::trianglelist_0)
        , projection_matrix(1.0F)
        , normalized_viewport_width(1.0F)
        , sampler(dev, sdl::filter::linear, sdl::filter::linear, sdl::sampler_address_mode::clamp_to_edge)
    {
    }

} // namespace nasrbrowse
