#include "tile_quad.hpp"
#include "map_view.hpp"
#include <cstdint>

namespace osect
{
    std::array<sdl::vertex_t2f_c4ub_v3f, 6> tile_quad(const tile_key& key, float u0, float v0, float u1, float v1)
    {
        const auto bounds = tile_bounds_meters(key.x, key.y, key.z);
        const auto x0 = static_cast<float>(bounds.x_min);
        const auto x1 = static_cast<float>(bounds.x_max);
        const auto y0 = static_cast<float>(bounds.y_min);
        const auto y1 = static_cast<float>(bounds.y_max);
        constexpr uint8_t white = 255;

        // Texture v runs north to south, so the south edge (y0) samples v1.
        const sdl::vertex_t2f_c4ub_v3f south_west{u0, v1, white, white, white, white, x0, y0, 0.0F};
        const sdl::vertex_t2f_c4ub_v3f south_east{u1, v1, white, white, white, white, x1, y0, 0.0F};
        const sdl::vertex_t2f_c4ub_v3f north_west{u0, v0, white, white, white, white, x0, y1, 0.0F};
        const sdl::vertex_t2f_c4ub_v3f north_east{u1, v0, white, white, white, white, x1, y1, 0.0F};
        return {south_west, south_east, north_west, north_west, south_east, north_east};
    }
} // namespace osect
