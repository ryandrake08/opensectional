#pragma once

#include <cstddef>
#include <functional>

namespace osect
{
    struct tile_key
    {
        int z = 0;
        int x = 0;
        int y = 0;

        bool operator==(const tile_key& other) const
        {
            return z == other.z && x == other.x && y == other.y;
        }
    };
} // namespace osect

namespace std
{
    template<>
    struct hash<osect::tile_key>
    {
        size_t operator()(const osect::tile_key& k) const
        {
            size_t h = 0;
            h ^= std::hash<int>()(k.z) + 0x9e3779b9 + (h << 6) + (h >> 2);
            h ^= std::hash<int>()(k.x) + 0x9e3779b9 + (h << 6) + (h >> 2);
            h ^= std::hash<int>()(k.y) + 0x9e3779b9 + (h << 6) + (h >> 2);
            return h;
        }
    };
} // namespace std
