#pragma once

#include <glm/glm.hpp>

namespace osect
{

    struct line_style
    {
        float line_width;   // total width in pixels
        float border_width; // border width in pixels (outside of path)
        float dash_length;  // dash length in pixels (0 = solid)
        float gap_length;   // gap length in pixels
        float r, g, b, a;   // line color (border is always black)
        float fill_width;   // border width on inside of path (0 = same as border_width)
    };

    // Circle primitive (center + radius in world-space Mercator meters)
    struct circle_data
    {
        glm::vec2 center;
        float radius;
        line_style style;
    };

} // namespace osect
