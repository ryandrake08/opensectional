#pragma once

#include "ini_config.hpp"
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace osect
{
    // Thrown by parse_cmdline on -h/--help, after usage has been
    // printed to stdout. main converts this to a successful exit.
    // Does not derive from std::exception so it bypasses the
    // top-level FATAL ERROR catch-all.
    struct help_requested
    {
    };

    // Parsed command-line options. An unset optional means "fall
    // back to a bundled asset or a built-in default".
    struct parsed_options
    {
        int verbosity = 0;
        std::optional<std::string> gpu_driver;
        std::optional<std::filesystem::path> tile_path;
        std::optional<std::filesystem::path> terrain_path;
        std::optional<std::filesystem::path> db_path;
        std::optional<std::filesystem::path> conf_path;
        bool offline = false;
        bool vsync = false;
        bool gpu_debug = false;
    };

    // Parse already-tokenized command-line arguments. cmdline[0] is
    // the program name (used for usage text); cmdline[1..] are
    // options. -h/--help prints usage to stdout and throws
    // help_requested; bad input prints usage to stderr and throws
    // std::runtime_error.
    parsed_options parse_cmdline(const std::vector<std::string>& cmdline);

    // Resolve the NASR database path: opts.db_path when given,
    // otherwise the osect.db bundled next to the executable or in
    // the current directory. Throws std::runtime_error (after
    // printing usage to stderr) when neither is available.
    std::filesystem::path resolve_db_path(const parsed_options& opts, const std::string& prog);

    // Resolve the basemap tile directory: opts.tile_path when given,
    // otherwise the bundled basemap. nullopt means "no basemap" —
    // tiles are optional.
    std::optional<std::filesystem::path> resolve_tile_path(const parsed_options& opts);

    // Resolve the terrain tile directory
    std::optional<std::filesystem::path> resolve_terrain_path(const parsed_options& opts, const ini_config& ini);

    // Build the ini_config: bundled defaults, then the per-user
    // file, then the optional --conf override, each merged on top.
    ini_config build_ini(const parsed_options& opts);

    // Resolve the GPU driver name from opts, defaulting to "vulkan".
    // Throws std::runtime_error when the requested driver is not
    // available on this platform / build configuration.
    std::string resolve_gpu_driver(const parsed_options& opts);
}
