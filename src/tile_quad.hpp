#pragma once

#include "tile_key.hpp"
#include <array>
#include <sdl/pipeline.hpp>

namespace osect
{
    // Six vertices (two triangles) covering the tile in Web Mercator meters,
    // textured with the (u0, v0)-(u1, v1) sub-rectangle: the whole texture
    // for the tile's own image, a smaller range when an ancestor's texture
    // stands in for it.
    std::array<sdl::vertex_t2f_c4ub_v3f, 6> tile_quad(const tile_key& key, float u0, float v0, float u1, float v1);
} // namespace osect
