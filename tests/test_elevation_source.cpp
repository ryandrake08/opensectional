#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "elevation_source.hpp"

#include <filesystem>
#include <fstream>
#include <optional>

namespace
{
    class temporary_tree
    {
        std::filesystem::path path_;

    public:
        temporary_tree(const std::string& name, const std::string& contents)
            : path_(std::filesystem::temp_directory_path() / name)
        {
            std::filesystem::remove_all(path_);
            std::filesystem::create_directories(path_);
            std::ofstream(path_ / "manifest.json") << contents;
        }

        ~temporary_tree()
        {
            std::filesystem::remove_all(path_);
        }

        const std::filesystem::path& path() const
        {
            return path_;
        }
    };

    std::string manifest(const std::string& dataset, int tile_pixels, int skirt_pixels, int min_zoom, int max_zoom,
                         double precision, const std::string& datum = "EGM2008",
                         const std::string& water_mask = {}, bool surface_model = false)
    {
        return "{\n"
               "  \"dataset\": \"" + dataset + "\",\n"
               "  \"dataset_display_name\": \"" + dataset + " display\",\n"
               "  \"source_version\": \"2026-01-01\",\n"
               "  \"attribution\": \"Test data\",\n"
               "  \"is_surface_model\": " + std::string(surface_model ? "true" : "false") + ",\n"
               "  \"vertical_datum\": \"" + datum + "\",\n"
               "  \"vertical_precision_m\": " + std::to_string(precision) + ",\n"
               "  \"tile_pixels\": " + std::to_string(tile_pixels) + ",\n"
               "  \"skirt_pixels\": " + std::to_string(skirt_pixels) + ",\n"
               "  \"min_zoom\": " + std::to_string(min_zoom) + ",\n"
               "  \"max_zoom\": " + std::to_string(max_zoom) +
               (water_mask.empty() ? "" : ",\n  \"water_mask\": " + water_mask) + "\n"
               "}\n";
    }

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

    void write_elevation_tile(const std::filesystem::path& path, bool high)
    {
        static constexpr uint8_t zero[] = {
            137, 80, 78, 71, 13, 10, 26, 10, 0, 0, 0, 13, 73, 72, 68, 82, 0, 0, 0, 1, 0, 0, 0, 1,
            8, 6, 0, 0, 0, 31, 21, 196, 137, 0, 0, 0, 13, 73, 68, 65, 84, 120, 156, 99, 104, 96, 96,
            248, 15, 0, 3, 4, 1, 128, 11, 131, 200, 20, 0, 0, 0, 0, 73, 69, 78, 68, 174, 66, 96, 130,
        };
        static constexpr uint8_t hundred[] = {
            137, 80, 78, 71, 13, 10, 26, 10, 0, 0, 0, 13, 73, 72, 68, 82, 0, 0, 0, 1, 0, 0, 0, 1,
            8, 6, 0, 0, 0, 31, 21, 196, 137, 0, 0, 0, 13, 73, 68, 65, 84, 120, 156, 99, 104, 72, 97,
            248, 15, 0, 4, 48, 1, 228, 10, 106, 56, 133, 0, 0, 0, 0, 73, 69, 78, 68, 174, 66, 96, 130,
        };
        std::filesystem::create_directories(path.parent_path());
        std::ofstream output(path, std::ios::binary);
        const uint8_t* png = high ? hundred : zero;
        output.write(reinterpret_cast<const char*>(png), sizeof(zero));
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

TEST_CASE("Elevation source uses the coarsest suitable pyramid level")
{
    temporary_tree tree("osect-elevation-source-pyramid", manifest("pyramid", 1, 0, 0, 1, 1.0));
    write_elevation_tile(tree.path() / "0" / "0" / "0.png", true);
    write_elevation_tile(tree.path() / "1" / "0" / "1.png", false);
    write_elevation_tile(tree.path() / "1" / "1" / "1.png", false);

    const osect::elevation_source source(tree.path());
    const std::optional<double> maximum = source.maximum_elevation_ft(-90.0, -180.0, 90.0, 180.0);

    REQUIRE(maximum);
    CHECK(*maximum == doctest::Approx(328.0839895013123));
}
