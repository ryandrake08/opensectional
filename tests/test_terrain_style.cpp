#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "ini_config.hpp"
#include "terrain_style.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace
{
    struct tmp_ini
    {
        std::string path;

        explicit tmp_ini(const std::string& contents)
        {
            auto tmpl = (std::filesystem::temp_directory_path() / "osect_terrain_style_XXXXXX").string();
            int fd = mkstemp(tmpl.data());
            REQUIRE(fd >= 0);
            close(fd);
            path = tmpl;
            std::ofstream(path) << contents;
        }

        ~tmp_ini()
        {
            std::remove(path.c_str());
        }

        ini_config load() const
        {
            return ini_config(std::filesystem::path(path));
        }
    };
}

TEST_CASE("defaults-only terrain_style")
{
    ini_config empty;
    osect::terrain_style s(empty);

    CHECK(s.mode == osect::terrain_shading::hypsometric);
    CHECK(s.opacity == 0.7F);
    CHECK(s.sun_azimuth_deg == 315.0F);
    CHECK(s.sun_altitude_deg == 45.0F);
    CHECK(s.vertical_exaggeration == 1.0F);
    CHECK(s.cruise_warning_ft == 500.0F);
    CHECK(s.cruise_caution_ft == 1000.0F);
    CHECK(s.cruise_clear_ft == 2000.0F);
    CHECK(s.gpu_tile_cache == 128);
    CHECK(s.cpu_tile_cache == 128);

    REQUIRE(s.ramp.size() >= 2);
    CHECK(s.ramp.front().elevation_m == 0.0F);
    CHECK(s.ramp.back().elevation_m == 4500.0F);
    for(std::size_t i = 1; i < s.ramp.size(); i++)
    {
        CHECK(s.ramp[i].elevation_m > s.ramp[i - 1].elevation_m);
    }

    CHECK(s.water_r == doctest::Approx(168.0F / 255.0F)); // divides by 255
    CHECK(s.water_g == doctest::Approx(200.0F / 255.0F));
    CHECK(s.water_b == doctest::Approx(224.0F / 255.0F));
}

TEST_CASE("terrain_style water_color override")
{
    tmp_ini ok("[terrain]\nwater_color = #3366cc\n");
    osect::terrain_style s(ok.load());
    CHECK(s.water_r == doctest::Approx(0x33 / 255.0F));
    CHECK(s.water_g == doctest::Approx(0x66 / 255.0F));
    CHECK(s.water_b == doctest::Approx(0xCC / 255.0F));

    tmp_ini bad("[terrain]\nwater_color = navy\n");
    CHECK_THROWS_AS(osect::terrain_style(bad.load()), std::runtime_error);
}

TEST_CASE("terrain_style overrides")
{
    tmp_ini ini(
        "[terrain]\n"
        "mode = cruise_relative\n"
        "opacity = 0.4\n"
        "sun_azimuth = 270\n"
        "sun_altitude = 30\n"
        "exaggeration = 1.5\n"
        "cruise_warning = 300\n"
        "cruise_caution = 800\n"
        "cruise_clear = 1600\n"
        "gpu_cache = 64\n"
        "cpu_cache = 32\n"
        "ramp = 0:#204030, 1000:#a0b070, 3000:#ffffff\n");
    osect::terrain_style s(ini.load());

    CHECK(s.mode == osect::terrain_shading::cruise_relative);
    CHECK(s.opacity == 0.4F);
    CHECK(s.sun_azimuth_deg == 270.0F);
    CHECK(s.sun_altitude_deg == 30.0F);
    CHECK(s.vertical_exaggeration == 1.5F);
    CHECK(s.cruise_warning_ft == 300.0F);
    CHECK(s.cruise_caution_ft == 800.0F);
    CHECK(s.cruise_clear_ft == 1600.0F);
    CHECK(s.gpu_tile_cache == 64);
    CHECK(s.cpu_tile_cache == 32);

    REQUIRE(s.ramp.size() == 3);
    CHECK(s.ramp[0].elevation_m == 0.0F);
    CHECK(s.ramp[1].elevation_m == 1000.0F);
    CHECK(s.ramp[2].elevation_m == 3000.0F);
    CHECK(s.ramp[0].r == doctest::Approx(0x20 / 255.0F)); // divides by 255
    CHECK(s.ramp[1].g == doctest::Approx(0xB0 / 255.0F));
    CHECK(s.ramp[2].b == 1.0F);
}

TEST_CASE("terrain_style short-hex ramp color")
{
    tmp_ini ini(
        "[terrain]\n"
        "ramp = 0:#123, 100:#abc\n");
    osect::terrain_style s(ini.load());

    REQUIRE(s.ramp.size() == 2);
    CHECK(s.ramp[0].r == doctest::Approx(0x11 / 255.0F)); // #123 -> 0x11,0x22,0x33
    CHECK(s.ramp[0].g == doctest::Approx(0x22 / 255.0F));
    CHECK(s.ramp[0].b == doctest::Approx(0x33 / 255.0F));
}

TEST_CASE("terrain_style rejects bad values")
{
    const auto rejects = [](const std::string& body)
    {
        tmp_ini ini("[terrain]\n" + body);
        CHECK_THROWS_AS(osect::terrain_style(ini.load()), std::runtime_error);
    };

    rejects("opacity = 1.5\n");
    rejects("opacity = -0.1\n");
    rejects("sun_azimuth = 400\n");
    rejects("sun_altitude = 120\n");
    rejects("exaggeration = 0\n");
    rejects("exaggeration = -2\n");
    rejects("mode = shaded\n");
    rejects("gpu_cache = 0\n");
    rejects("cpu_cache = 0\n");
    rejects("cruise_warning = 1500\n"); // exceeds default caution (1000)
    rejects("ramp = 1000:#ffffff\n");   // single stop
    rejects("ramp = 1000:#fff, 500:#000\n"); // descending
    rejects("ramp = 1000:teal\n");           // not a hex color
    rejects("ramp = high:#fff, low:#000\n"); // non-numeric elevation
}
