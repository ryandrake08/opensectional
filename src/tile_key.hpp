#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>

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

        // The same tile with its column folded into [0, 2^z), for keys whose
        // x has run past the antimeridian.
        tile_key wrapped() const
        {
            const int count = 1 << z;
            return {z, ((x % count) + count) % count, y};
        }

        // The tile one zoom level up that contains this one. z must be > 0.
        tile_key parent() const
        {
            return {z - 1, wrapped().x / 2, y / 2};
        }
    };

    // root/z/x/y.png, the layout of the basemap and terrain tile trees,
    // with x wrapped.
    inline std::filesystem::path tile_file_path(const std::filesystem::path& root, const tile_key& key)
    {
        const tile_key w = key.wrapped();
        return root / std::to_string(w.z) / std::to_string(w.x) / (std::to_string(w.y) + ".png");
    }
} // namespace osect

namespace std
{
    template <>
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
