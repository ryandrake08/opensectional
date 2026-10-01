#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "elevation_source.hpp"
#include "elevation_test_tree.hpp"

#include <filesystem>
#include <fstream>
#include <optional>

namespace
{
    using osect::test::manifest;
    using osect::test::temporary_tree;
    using osect::test::write_elevation_tile;

    void write_tile(const std::filesystem::path& path)
    {
        static constexpr uint8_t png[] = {
            137, 80, 78, 71, 13, 10, 26, 10, 0, 0, 0, 13, 73, 72, 68, 82, 0, 0, 0, 1, 0, 0, 0, 1,
            8, 6, 0, 0, 0, 31, 21, 196, 137, 0, 0, 0, 13, 73, 68, 65, 84, 120, 156, 99, 248, 207, 192,
            240, 31, 0, 5, 0, 1, 255, 137, 153, 61, 29, 0, 0, 0, 0, 73, 69, 78, 68, 174, 66, 96, 130,
        };
        std::filesystem::create_directories(path.parent_path());
        std::ofstream output(path, std::ios::binary);
        output.write(reinterpret_cast<const char*>(png), sizeof(png));
    }
}

TEST_CASE("Elevation source reads dataset-agnostic manifests")
{
    temporary_tree first("osect-elevation-source-first", manifest("first", 256, 2, 0, 6, 1.0));
    temporary_tree second("osect-elevation-source-second", manifest("second", 512, 4, 3, 11, 0.25, "EGM2008", {}, true));

    const osect::elevation_source first_source(first.path());
    const osect::elevation_source second_source(second.path());

    CHECK(first_source.available());
    CHECK(first_source.max_zoom() == 6);
    CHECK(first_source.tile_size() == 256);
    CHECK(first_source.skirt() == 2);
    CHECK(first_source.vertical_precision_m() == 1.0);
    CHECK(first_source.attribution() == "Test data");
    CHECK_FALSE(first_source.is_surface_model());
    CHECK(first_source.data_source_row().info == "first display 2026-01-01");

    CHECK(second_source.available());
    CHECK(second_source.max_zoom() == 11);
    CHECK(second_source.tile_size() == 512);
    CHECK(second_source.skirt() == 4);
    CHECK(second_source.vertical_precision_m() == 0.25);
    CHECK(second_source.attribution() == "Test data");
    CHECK(second_source.is_surface_model());
    CHECK(second_source.data_source_row().info == "second display 2026-01-01");
}

TEST_CASE("Elevation source reads the optional water_mask block")
{
    const std::string wm = "{\n"
                           "    \"source\": \"gshhg\",\n"
                           "    \"classes\": {\"0\": \"none\", \"1\": \"ocean\", \"2\": \"lake\"},\n"
                           "    \"min_zoom\": 0,\n"
                           "    \"max_zoom\": 8,\n"
                           "    \"bbox\": [-180, -60, 180, 84]\n"
                           "  }";
    temporary_tree with("osect-elevation-source-water", manifest("wet", 256, 2, 0, 11, 1.0, "EGM2008", wm));
    temporary_tree without("osect-elevation-source-nowater", manifest("dry", 256, 2, 0, 6, 1.0));

    const osect::elevation_source wet(with.path());
    // The nested min_zoom must not collide with the top-level one (0..11 here).
    CHECK(wet.has_water_mask());
    CHECK(wet.water_min_zoom() == 0);
    CHECK(wet.water_max_zoom() == 8);
    CHECK(wet.max_zoom() == 11);

    CHECK_FALSE(osect::elevation_source(without.path()).has_water_mask());
}

TEST_CASE("Elevation source builds tile paths from its tree layout")
{
    const std::string wm = "{\"source\": \"gshhg\", \"classes\": {\"0\": \"none\"}, \"min_zoom\": 0, \"max_zoom\": 6, "
                           "\"bbox\": [-180, -60, 180, 84]}";
    temporary_tree with("osect-elevation-source-paths-water", manifest("wet", 256, 2, 0, 6, 1.0, "EGM2008", wm));
    temporary_tree without("osect-elevation-source-paths-dry", manifest("dry", 256, 2, 0, 6, 1.0));

    const osect::elevation_source wet(with.path());
    const osect::elevation_source dry(without.path());

    CHECK(wet.tile_path({3, -1, 2}) == with.path() / "3" / "7" / "2.png");
    CHECK(wet.water_tile_path({3, -1, 2}) == with.path() / "water" / "3" / "7" / "2.png");
    CHECK(dry.water_tile_path({3, 1, 2}).empty());
}

TEST_CASE("Missing terrain tree is unavailable")
{
    const osect::elevation_source source("missing-terrain-tree");

    CHECK_FALSE(source.available());
    CHECK(source.data_source_row().info == "Terrain unavailable");
}

TEST_CASE("Elevation source decodes escaped manifest strings")
{
    temporary_tree tree("osect-elevation-source-escaped",
                        "{\n"
                        "  \"dataset\": \"escaped\",\n"
                        "  \"dataset_display_name\": \"escaped display\",\n"
                        "  \"source_version\": \"2026-01-01\",\n"
                        "  \"attribution\": \"Agency \\\"quoted\\\" text\",\n"
                        "  \"is_surface_model\": false,\n"
                        "  \"vertical_datum\": \"EGM2008\",\n"
                        "  \"vertical_precision_m\": 1,\n"
                        "  \"tile_pixels\": 256,\n"
                        "  \"skirt_pixels\": 2,\n"
                        "  \"min_zoom\": 0,\n"
                        "  \"max_zoom\": 6\n"
                        "}\n");

    const osect::elevation_source source(tree.path());

    CHECK(source.attribution() == "Agency \"quoted\" text");
}

TEST_CASE("Elevation source decodes \\u escapes in manifest strings")
{
    // e-acute (2 UTF-8 bytes), em dash (3), and U+1F5FA as a surrogate pair (4).
    temporary_tree tree("osect-elevation-source-unicode",
                        "{\n"
                        "  \"dataset\": \"unicode\",\n"
                        "  \"dataset_display_name\": \"Donn\\u00E9es \\u2014 \\ud83d\\uddfa\",\n"
                        "  \"source_version\": \"2026-01-01\",\n"
                        "  \"attribution\": \"Test data\",\n"
                        "  \"is_surface_model\": false,\n"
                        "  \"vertical_datum\": \"EGM2008\",\n"
                        "  \"vertical_precision_m\": 1,\n"
                        "  \"tile_pixels\": 256,\n"
                        "  \"skirt_pixels\": 2,\n"
                        "  \"min_zoom\": 0,\n"
                        "  \"max_zoom\": 6\n"
                        "}\n");

    const osect::elevation_source source(tree.path());

    CHECK(source.data_source_row().info == "Donn\xC3\xA9" "es \xE2\x80\x94 \xF0\x9F\x97\xBA 2026-01-01");
}

TEST_CASE("Elevation source rejects malformed \\u escapes")
{
    for(const std::string escape : {"\\u12G4", "\\u12", "\\ud83d", "\\ud83d\\u0041", "\\uddfa"})
    {
        CAPTURE(escape);
        std::string json = manifest("bad", 256, 2, 0, 6, 1.0);
        json.replace(json.find("Test data"), 9, escape);
        temporary_tree tree("osect-elevation-source-bad-unicode", json);

        CHECK_THROWS_AS(osect::elevation_source(tree.path()), std::runtime_error);
    }
}

TEST_CASE("Elevation source rejects unsupported vertical data")
{
    temporary_tree tree("osect-elevation-source-datum", manifest("bad", 256, 2, 0, 6, 1.0, "NAVD88"));

    CHECK_THROWS_AS(osect::elevation_source(tree.path()), std::runtime_error);
}

TEST_CASE("Elevation source caches point-query tiles")
{
    temporary_tree tree("osect-elevation-source-query", manifest("query", 1, 0, 1, 1, 1.0));
    const std::filesystem::path west = tree.path() / "1" / "0" / "1.png";
    const std::filesystem::path east = tree.path() / "1" / "1" / "1.png";
    write_tile(west);
    write_tile(east);

    const osect::elevation_source source(tree.path(), 1);
    const std::optional<double> west_elevation = source.elevation_ft(0.0, -90.0, 1);
    REQUIRE(west_elevation);
    CHECK(*west_elevation == doctest::Approx(106666.66666666667));

    std::filesystem::remove(west);
    CHECK(source.elevation_ft(0.0, -90.0, 1) == west_elevation);

    REQUIRE(source.elevation_ft(0.0, 90.0, 1));
    CHECK_FALSE(source.elevation_ft(0.0, -90.0, 1));
    CHECK_FALSE(source.elevation_ft(0.0, 0.0, 0));
}

TEST_CASE("Elevation source finds the conservative bbox maximum")
{
    temporary_tree tree("osect-elevation-source-maximum", manifest("maximum", 1, 0, 1, 1, 1.0));
    write_elevation_tile(tree.path() / "1" / "0" / "1.png", false);
    write_elevation_tile(tree.path() / "1" / "1" / "1.png", true);

    const osect::elevation_source source(tree.path());
    const std::optional<double> maximum = source.maximum_elevation_ft(-10.0, -90.0, 10.0, 90.0);

    REQUIRE(maximum);
    CHECK(*maximum == doctest::Approx(328.0839895013123));
    CHECK_FALSE(source.maximum_elevation_ft(-10.0, -90.0, 10.0, -90.0));
}

TEST_CASE("Elevation source reads the coarsest level spanning four texels")
{
    // 1-pixel tiles: the world is 2 texels wide at zoom 1 and 4 at zoom 2.
    temporary_tree tree("osect-elevation-source-pyramid", manifest("pyramid", 1, 0, 1, 3, 1.0));
    for(int x = 0; x < 2; ++x)
    {
        for(int y = 0; y < 2; ++y)
        {
            write_elevation_tile(tree.path() / "1" / std::to_string(x) / (std::to_string(y) + ".png"), true);
        }
    }
    for(int x = 0; x < 4; ++x)
    {
        for(int y = 0; y < 4; ++y)
        {
            write_elevation_tile(tree.path() / "2" / std::to_string(x) / (std::to_string(y) + ".png"), false);
        }
    }

    const osect::elevation_source source(tree.path());
    const std::optional<double> maximum = source.maximum_elevation_ft(-90.0, -180.0, 90.0, 180.0);

    REQUIRE(maximum);
    CHECK(*maximum == 0.0);
}
