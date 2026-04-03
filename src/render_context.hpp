#pragma once

#include <glm/glm.hpp>
#include <sdl/sampler.hpp>

namespace sdl
{
    class device;
}

namespace nasrbrowse
{
    enum class render_pass_id
    {
        trianglelist_0,
        textured_trianglelist_0,
        line_sdf_0,
        text_labels_0
    };

    struct render_context
    {
        explicit render_context(const sdl::device& dev);

        render_pass_id current_pass;
        glm::mat4 projection_matrix;
        float normalized_viewport_width;
        sdl::sampler sampler;
    };

} // namespace nasrbrowse
