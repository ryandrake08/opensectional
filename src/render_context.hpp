#pragma once

#include <glm/glm.hpp>

namespace nasrbrowse
{
    enum class render_pass_id
    {
        trianglelist_0,
        textured_trianglelist_0,
        polygon_fill_0,
        line_sdf_0,
        text_labels_0
    };

    struct render_context
    {
        render_pass_id current_pass = render_pass_id::trianglelist_0;
        glm::mat4 projection_matrix{1.0F};
        float normalized_viewport_width = 1.0F;
    };

} // namespace nasrbrowse
