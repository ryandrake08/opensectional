#include "app_options.hpp"

#include <iostream>
#include <sdl/filesystem.hpp>
#include <sdl/log.hpp>
#include <stdexcept>

namespace osect
{
    namespace
    {
        void print_usage(std::ostream& out, const std::string& prog)
        {
            out << "Usage: " << prog << " [options]\n"
                << "  -h, --help                 Show this help and exit\n"
                << "  -v, -vv, -vvv              Increase verbosity\n"
                << "  -g, --gpu <driver>         GPU driver: vulkan"
#ifdef __APPLE__
                << ", metal"
#endif
#ifdef OSECT_HAVE_DXIL
                << ", direct3d12"
#endif
                << "\n"
                << "  --vsync                    Enable vsync (default: off, lowest latency)\n"
                << "  --gpu_debug                Enable GPU debug/validation (Vulkan: requires LunarG SDK)\n"
                << "  -b, --basemap <path>       Basemap tile directory\n"
                << "  -t, --terrain <path>       Terrain tile directory\n"
                << "  -d, --database <osect.db>  NASR SQLite database\n"
                << "  -c, --conf <osect.ini>     Override INI (optional; layered over defaults)\n"
                << "  --offline                  Skip all network fetches; use cached ephemeral data only\n"
                << "\n"
                << "When -b/-d are omitted, the asset is loaded from next to the\n"
                << "executable (installer layout) or the current directory. -c is\n"
                << "fully optional; chart-style and routing defaults are baked in,\n"
                << "with bundled / per-user / -c override files cascading on top.\n";
        }
    }

    parsed_options parse_cmdline(const std::vector<std::string>& cmdline)
    {
        sdl::log_info(OSECT_APP_DISPLAY_NAME " " OSECT_APP_VERSION " (built " __DATE__ " " __TIME__ ")");

        parsed_options opts;
        const auto& prog = cmdline.empty() ? std::string{"osect"} : cmdline[0];

        for(std::size_t i = 1; i < cmdline.size(); ++i)
        {
            const auto& arg = cmdline[i];

            auto need_value = [&](const char* flag) -> const std::string&
            {
                if(i + 1 >= cmdline.size())
                {
                    throw std::runtime_error(std::string("Missing value for ") + flag);
                }
                return cmdline[++i];
            };

            if(arg == "-h" || arg == "--help")
            {
                print_usage(std::cout, prog);
                throw help_requested{};
            }
            else if(arg.size() >= 2 && arg[0] == '-' && arg[1] == 'v')
            {
                const auto* p = arg.c_str() + 1;
                while(*p == 'v')
                {
                    opts.verbosity++;
                    p++;
                }
                if(*p != '\0')
                {
                    print_usage(std::cerr, prog);
                    throw std::runtime_error("Unknown option: " + arg);
                }
            }
            else if(arg == "-g" || arg == "--gpu")
            {
                opts.gpu_driver = need_value("--gpu");
            }
            else if(arg == "--vsync")
            {
                opts.vsync = true;
                sdl::log_info("Vsync enabled");
            }
            else if(arg == "--gpu_debug")
            {
                opts.gpu_debug = true;
                sdl::log_info("GPU debug/validation enabled");
            }
            else if(arg == "-b" || arg == "--basemap")
            {
                opts.tile_path = need_value("--basemap");
                sdl::log_info("Basemap directory: " + opts.tile_path->string());
            }
            else if(arg == "-t" || arg == "--terrain")
            {
                opts.terrain_path = need_value("--terrain");
                sdl::log_info("Terrain directory: " + opts.terrain_path->string());
            }
            else if(arg == "-d" || arg == "--database")
            {
                opts.db_path = need_value("--database");
                sdl::log_info("Database: " + opts.db_path->string());
            }
            else if(arg == "-c" || arg == "--conf")
            {
                opts.conf_path = need_value("--conf");
                sdl::log_info("Override INI: " + opts.conf_path->string());
            }
            else if(arg == "--offline")
            {
                opts.offline = true;
                sdl::log_info("Running in offline mode");
            }
            else
            {
                print_usage(std::cerr, prog);
                throw std::runtime_error("Unknown option: " + arg);
            }
        }

        return opts;
    }

    std::filesystem::path resolve_db_path(const parsed_options& opts, const std::string& prog)
    {
        if(opts.db_path)
        {
            return *opts.db_path;
        }
        auto resolved = sdl::resolve_bundled_asset("osect.db");
        if(resolved.empty())
        {
            print_usage(std::cerr, prog);
            throw std::runtime_error(
                "No osect.db supplied and none found next to the executable or in the current directory.");
        }
        return resolved;
    }

    std::optional<std::filesystem::path> resolve_tile_path(const parsed_options& opts)
    {
        if(opts.tile_path)
        {
            return *opts.tile_path;
        }
        // Tiles are optional — nullopt means "no basemap".
        auto bundled = sdl::resolve_bundled_asset("basemap");
        if(bundled.empty())
        {
            return std::nullopt;
        }
        return std::filesystem::path{std::move(bundled)};
    }

    std::optional<std::filesystem::path> resolve_terrain_path(const parsed_options& opts)
    {
        if(opts.terrain_path)
        {
            return *opts.terrain_path;
        }
        auto bundled = sdl::resolve_bundled_asset("terrain");
        if(bundled.empty())
        {
            return std::nullopt;
        }
        const std::filesystem::path root{std::move(bundled)};
        if(std::filesystem::exists(root / "manifest.json"))
        {
            return root;
        }

        std::optional<std::filesystem::path> dataset;
        for(const auto& entry : std::filesystem::directory_iterator(root))
        {
            if(!entry.is_directory() || !std::filesystem::exists(entry.path() / "manifest.json"))
            {
                continue;
            }
            if(dataset)
            {
                return std::nullopt;
            }
            dataset = entry.path();
        }
        return dataset;
    }

    ini_config build_ini(const parsed_options& opts)
    {
        ini_config ini;
        auto bundled = sdl::resolve_bundled_asset("osect.ini");
        if(!bundled.empty())
        {
            sdl::log_info("ini merge: bundled " + bundled);
            ini.merge(ini_config(bundled));
        }
        auto user = sdl::resolve_user_asset({OSECT_BUNDLE_IDENTIFIER, OSECT_APP_NAME}, "osect.ini");
        if(!user.empty())
        {
            sdl::log_info("ini merge: user " + user);
            ini.merge(ini_config(user));
        }
        if(opts.conf_path)
        {
            sdl::log_info("ini merge: --conf " + opts.conf_path->string());
            ini.merge(ini_config(*opts.conf_path));
        }
        return ini;
    }

    std::string resolve_gpu_driver(const parsed_options& opts)
    {
        // Default to Vulkan for cross-platform parity rather than
        // letting SDL auto-pick its per-platform default (Metal on
        // macOS, D3D12 on Windows when DXIL is built, Vulkan on Linux).
        if(!opts.gpu_driver)
        {
            return "vulkan";
        }
        const std::string& d = *opts.gpu_driver;
        if(d == "vulkan")
        {
            return d;
        }
        if(d == "metal")
        {
#ifdef __APPLE__
            return d;
#else
            throw std::runtime_error("--gpu metal not available: Metal is supported on macOS only.");
#endif
        }
        if(d == "direct3d12")
        {
#ifdef OSECT_HAVE_DXIL
            return d;
#elif defined(_WIN32)
            throw std::runtime_error(
                "--gpu direct3d12 not available: this build was configured without D3D12 support."
                " Rebuild with -DOSECT_ENABLE_D3D12=ON and dxc on PATH (or in $VULKAN_SDK/bin).");
#else
            throw std::runtime_error("--gpu direct3d12 not available: D3D12 is supported on Windows only.");
#endif
        }
        throw std::runtime_error("--gpu " + d + " not recognized. Valid drivers: vulkan, metal, direct3d12.");
    }
}
