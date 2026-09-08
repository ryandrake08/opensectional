#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "elevation_tile.hpp"
#include <array>
#include <cmath>

namespace
{
    void set_terrarium(std::array<uint8_t, 16>& pixels, int x, int y, double elevation_m)
    {
        const int encoded = static_cast<int>(std::round((elevation_m + 32768.0) * 256.0));
        const size_t offset = static_cast<size_t>((y * 2 + x) * 4);
        pixels[offset] = static_cast<uint8_t>(encoded >> 16);
        pixels[offset + 1] = static_cast<uint8_t>(encoded >> 8);
        pixels[offset + 2] = static_cast<uint8_t>(encoded);
        pixels[offset + 3] = 255;
    }
}

TEST_CASE("Terrarium pixels decode to metres")
{
    std::array<uint8_t, 16> pixels = {};
    set_terrarium(pixels, 0, 0, 123.5);
    set_terrarium(pixels, 1, 0, -250.25);
    set_terrarium(pixels, 0, 1, 0.0);
    set_terrarium(pixels, 1, 1, 32767.0);

    const osect::elevation_tile tile = osect::elevation_tile::from_rgba(2, 2, pixels.data(), 8);

    CHECK(tile.sample_m(0.0, 0.0) == 123.5F);
    CHECK(tile.sample_m(1.0, 0.0) == -250.25F);
    CHECK(tile.sample_m(0.0, 1.0) == 0.0F);
    CHECK(tile.sample_m(1.0, 1.0) == 32767.0F);
}

TEST_CASE("Elevation tile loads a PNG")
{
    const osect::elevation_tile tile = osect::elevation_tile::load("osect.png");

    CHECK(tile.width() == 1024);
    CHECK(tile.height() == 1024);
}

TEST_CASE("Elevation samples bilinearly")
{
    std::array<uint8_t, 16> pixels = {};
    set_terrarium(pixels, 0, 0, 100.0);
    set_terrarium(pixels, 1, 0, 200.0);
    set_terrarium(pixels, 0, 1, 300.0);
    set_terrarium(pixels, 1, 1, 400.0);

    const osect::elevation_tile tile = osect::elevation_tile::from_rgba(2, 2, pixels.data(), 8);

    CHECK(tile.sample_m(0.5, 0.5) == 250.0F);
    CHECK(tile.sample_m(-1.0, 0.0) == 100.0F);
    CHECK(tile.sample_m(2.0, 1.0) == 400.0F);
}

TEST_CASE("No-data propagates through elevation samples")
{
    std::array<uint8_t, 16> pixels = {};
    set_terrarium(pixels, 0, 0, 100.0);
    set_terrarium(pixels, 1, 0, 200.0);
    set_terrarium(pixels, 0, 1, 300.0);

    const osect::elevation_tile tile = osect::elevation_tile::from_rgba(2, 2, pixels.data(), 8);

    CHECK(std::isnan(tile.sample_m(1.0, 1.0)));
    CHECK(std::isnan(tile.sample_m(0.5, 0.5)));
}

TEST_CASE("Elevation tile finds a maximum over a pixel rectangle")
{
    std::array<uint8_t, 16> pixels = {};
    set_terrarium(pixels, 0, 0, 100.0);
    set_terrarium(pixels, 1, 0, 200.0);
    set_terrarium(pixels, 0, 1, 300.0);
    set_terrarium(pixels, 1, 1, 400.0);

    const osect::elevation_tile tile = osect::elevation_tile::from_rgba(2, 2, pixels.data(), 8);

    CHECK(tile.maximum_m(0, 0, 1, 0) == 200.0F);
    CHECK(tile.maximum_m(0, 0, 1, 1) == 400.0F);
}

TEST_CASE("Elevation tile quantizes metres for R16 upload")
{
    std::array<uint8_t, 16> pixels = {};
    set_terrarium(pixels, 0, 0, -999.0);
    set_terrarium(pixels, 1, 0, 0.0);
    set_terrarium(pixels, 0, 1, 512.0);

    const osect::elevation_tile tile = osect::elevation_tile::from_rgba(2, 2, pixels.data(), 8);

    CHECK(tile.quantized_m() == std::vector<uint16_t>{1, 1000, 1512, 0});
}
