#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace osect
{
    class elevation_tile
    {
        int width_ = 0;
        int height_ = 0;
        std::vector<float> elevations_m_;

        elevation_tile(int width, int height, std::vector<float> elevations_m);

    public:
        // Decodes a Terrarium PNG into metre samples.
        static elevation_tile load(const std::filesystem::path& path);
        // Decodes tightly packed RGBA Terrarium pixels.
        static elevation_tile from_rgba(int width, int height, const uint8_t* rgba, size_t stride);

        int width() const;
        int height() const;
        // Bilinearly samples elevation in tile-pixel coordinates.
        float sample_m(double x, double y) const;
        // Finds the greatest valid sample in an inclusive pixel rectangle.
        float maximum_m(int x_min, int y_min, int x_max, int y_max) const;
        // Encodes whole metres for an R16_UNORM texture with a -1000 m bias.
        std::vector<uint16_t> quantized_m() const;
    };
} // namespace osect
