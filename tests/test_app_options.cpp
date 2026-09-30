#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "app_options.hpp"
#include "sdl/filesystem.hpp"

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

using namespace osect;

TEST_CASE("parse_cmdline: no options yields defaults")
{
    auto o = parse_cmdline({"osect"});
    CHECK(o.verbosity == 0);
    CHECK(!o.gpu_driver);
    CHECK(!o.tile_path);
    CHECK(!o.terrain_path);
    CHECK(!o.db_path);
    CHECK(!o.conf_path);
    CHECK(!o.offline);
    CHECK(!o.vsync);
    CHECK(!o.gpu_debug);
}

TEST_CASE("parse_cmdline: an empty command line is tolerated")
{
    CHECK(parse_cmdline({}).verbosity == 0);
}

TEST_CASE("parse_cmdline: -h / --help throws help_requested")
{
    CHECK_THROWS_AS(parse_cmdline({"osect", "-h"}), help_requested);
    CHECK_THROWS_AS(parse_cmdline({"osect", "--help"}), help_requested);
}

TEST_CASE("parse_cmdline: verbosity counts the v's")
{
    CHECK(parse_cmdline({"osect", "-v"}).verbosity == 1);
    CHECK(parse_cmdline({"osect", "-vv"}).verbosity == 2);
    CHECK(parse_cmdline({"osect", "-vvv"}).verbosity == 3);
}

TEST_CASE("parse_cmdline: value-taking flags capture their argument")
{
    auto o = parse_cmdline({"osect", "-g", "metal", "-d", "x.db", "-b", "tiles", "-t", "terrain", "-c", "y.ini"});
    REQUIRE(o.gpu_driver);
    CHECK(*o.gpu_driver == "metal");
    REQUIRE(o.db_path);
    CHECK(*o.db_path == "x.db");
    REQUIRE(o.tile_path);
    CHECK(*o.tile_path == "tiles");
    REQUIRE(o.terrain_path);
    CHECK(*o.terrain_path == "terrain");
    REQUIRE(o.conf_path);
    CHECK(*o.conf_path == "y.ini");
}

TEST_CASE("parse_cmdline: long flag names are accepted")
{
    auto o = parse_cmdline({"osect", "--gpu", "vulkan", "--database", "x.db", "--basemap", "t", "--terrain", "terrain", "--conf", "c.ini"});
    CHECK(*o.gpu_driver == "vulkan");
    CHECK(*o.db_path == "x.db");
    CHECK(*o.tile_path == "t");
    CHECK(*o.terrain_path == "terrain");
    CHECK(*o.conf_path == "c.ini");
}

TEST_CASE("parse_cmdline: boolean flags set their fields")
{
    auto o = parse_cmdline({"osect", "--offline", "--vsync", "--gpu_debug"});
    CHECK(o.offline);
    CHECK(o.vsync);
    CHECK(o.gpu_debug);
}

TEST_CASE("parse_cmdline: an unknown option throws")
{
    CHECK_THROWS_AS(parse_cmdline({"osect", "--bogus"}), std::runtime_error);
}

TEST_CASE("parse_cmdline: a value-taking flag with no value throws")
{
    CHECK_THROWS_AS(parse_cmdline({"osect", "-g"}), std::runtime_error);
    CHECK_THROWS_AS(parse_cmdline({"osect", "-d"}), std::runtime_error);
    CHECK_THROWS_AS(parse_cmdline({"osect", "-c"}), std::runtime_error);
}

TEST_CASE("parse_cmdline: a malformed verbosity cluster throws")
{
    CHECK_THROWS_AS(parse_cmdline({"osect", "-vx"}), std::runtime_error);
}

TEST_CASE("resolve_gpu_driver: defaults to vulkan and echoes an explicit vulkan")
{
    parsed_options o;
    CHECK(resolve_gpu_driver(o) == "vulkan");
    o.gpu_driver = "vulkan";
    CHECK(resolve_gpu_driver(o) == "vulkan");
}

TEST_CASE("resolve_gpu_driver: an unrecognized driver throws")
{
    parsed_options o;
    o.gpu_driver = "softpipe";
    CHECK_THROWS_AS(resolve_gpu_driver(o), std::runtime_error);
}

TEST_CASE("resolve_terrain_path: --terrain is used verbatim")
{
    parsed_options options;
    options.terrain_path = "command-line-terrain";

    CHECK(resolve_terrain_path(options) == std::filesystem::path("command-line-terrain"));
}

TEST_CASE("resolve_bundled_asset: finds the Linux package layout under ../share/<dir>/")
{
    // base_path() is the directory holding this test binary; plant an asset
    // in its ../share/<unique dir>/ and remove the tree afterwards.
    const std::filesystem::path base = sdl::base_path();
    REQUIRE(!base.empty());
    const std::string share_dir = "osect_test_share_lookup";
    const auto dir = base / ".." / "share" / share_dir;
    std::filesystem::create_directories(dir);
    std::ofstream(dir / "probe.txt") << "x";

    const auto found = sdl::resolve_bundled_asset("probe.txt", share_dir.c_str());
    CHECK(std::filesystem::equivalent(found, dir / "probe.txt"));
    // Without a share dir the FHS location isn't searched.
    CHECK(sdl::resolve_bundled_asset("probe.txt").empty());

    std::filesystem::remove_all(dir);
    std::error_code ec;
    std::filesystem::remove(base / ".." / "share", ec); // only if now empty
}

// Plants terrain/ under the test binary's ../share/<app>/, which the bundled
// asset lookup searches before the working directory (the repo root, which
// may hold a real terrain/), and removes it afterwards.
struct planted_terrain
{
    std::filesystem::path share = std::filesystem::path(sdl::base_path()) / ".." / "share";
    std::filesystem::path dir = share / OSECT_APP_NAME / "terrain";

    explicit planted_terrain(bool with_manifest)
    {
        std::filesystem::create_directories(dir);
        if(with_manifest)
        {
            std::ofstream(dir / "manifest.json") << "{}";
        }
    }
    ~planted_terrain()
    {
        std::filesystem::remove_all(share / OSECT_APP_NAME);
        std::error_code ec;
        std::filesystem::remove(share, ec); // only if now empty
    }
    planted_terrain(const planted_terrain&) = delete;
    planted_terrain& operator=(const planted_terrain&) = delete;
};

TEST_CASE("resolve_terrain_path: bundled terrain/ with a manifest is used")
{
    planted_terrain terrain(true);
    auto resolved = resolve_terrain_path(parsed_options{});
    REQUIRE(resolved);
    CHECK(std::filesystem::equivalent(*resolved, terrain.dir));
}

TEST_CASE("resolve_terrain_path: bundled terrain/ without a manifest means no terrain")
{
    planted_terrain terrain(false);
    CHECK(!resolve_terrain_path(parsed_options{}));
}
