#include "program.hpp"
#include "ephemeral_data.hpp"
#include "ini_config.hpp"
#include "map_widget.hpp"
#include "nasr_database.hpp"
#include "route_plan_config.hpp"
#include "route_planner.hpp"
#include "route_submitter.hpp"
#include "ui_overlay.hpp"
#include <imgui/context.hpp>
#include <iostream>
#include <optional>
#include <sdl/command_buffer.hpp>
#include <sdl/copy_pass.hpp>
#include <sdl/device.hpp>
#include <sdl/event.hpp>
#include <sdl/filesystem.hpp>
#include <sdl/instance.hpp>
#include <sdl/render_pass.hpp>
#include <sdl/texture.hpp>
#include <sdl/timer.hpp>
#include <sdl/window.hpp>
#include <stdexcept>

namespace osect
{
    namespace
    {
        constexpr auto SEARCH_RESULT_LIMIT = 12;

        void print_usage(std::ostream& out, const std::string& prog)
        {
            out << "Usage: " << prog << " [options]\n"
                << "  -h, --help                 Show this help and exit\n"
                << "  -v, -vv, -vvv              Increase verbosity\n"
#ifdef OSECT_HAVE_DXIL
                << "  -g, --gpu <driver>         GPU driver: vulkan, metal, direct3d12\n"
#else
                << "  -g, --gpu <driver>         GPU driver: vulkan, metal\n"
#endif
                << "  -b, --basemap <path>       Basemap tile directory\n"
                << "  -d, --database <osect.db>   NASR SQLite database\n"
                << "  -c, --conf <osect.ini>     Override INI (optional; layered over defaults)\n"
                << "  --offline                  Skip all network fetches; use cached ephemeral data only\n"
                << "\n"
                << "When -b/-d are omitted, the asset is loaded from next to the\n"
                << "executable (installer layout) or the current directory. -c is\n"
                << "fully optional; chart-style and routing defaults are baked in,\n"
                << "with bundled / per-user / -c override files cascading on top.\n";
        }

        struct parsed_options
        {
            int verbosity = 0;
            std::optional<std::string> gpu_driver;
            std::optional<std::string> tile_path;
            std::optional<std::string> db_path;
            std::optional<std::string> conf_path;
            bool offline = false;
        };

        // Parse already-tokenized command-line arguments. argv[0] is
        // the program name (used for usage text); argv[1..] are
        // options. -h prints to stdout and exits with success; bad
        // input prints to stderr and throws std::runtime_error.
        parsed_options parse_cmdline(const std::vector<std::string>& cmdline)
        {
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
                    throw program::help_requested{};
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
                else if(arg == "-b" || arg == "--basemap")
                {
                    opts.tile_path = need_value("--basemap");
                }
                else if(arg == "-d" || arg == "--database")
                {
                    opts.db_path = need_value("--database");
                }
                else if(arg == "-c" || arg == "--conf")
                {
                    opts.conf_path = need_value("--conf");
                }
                else if(arg == "--offline")
                {
                    opts.offline = true;
                }
                else
                {
                    print_usage(std::cerr, prog);
                    throw std::runtime_error("Unknown option: " + arg);
                }
            }

            return opts;
        }
    }

    struct program::impl
    {
        parsed_options opts;

        // Resolved paths (filled from opts and bundled-asset lookup
        // before subsystems are constructed).
        std::string db_path;
        std::string tile_path;

        sdl::instance sdl_ctx;
        sdl::window win;
        sdl::device dev;
        imgui::context imgui_ctx;
        sdl::event_manager event_mgr;
        ephemeral_data eph;
        ini_config ini;
        map_widget map;
        nasr_database planner_db;
        route_planner planner;
        route_submitter submitter;
        route_plan_options plan_options;
        ui_overlay ui;
        std::vector<data_source> static_sources;

        static std::string resolve_db_path(const parsed_options& opts, const std::string& prog)
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

        static std::string resolve_tile_path(const parsed_options& opts)
        {
            if(opts.tile_path)
            {
                return *opts.tile_path;
            }
            // Tiles are optional — empty string means "no basemap".
            return sdl::resolve_bundled_asset("basemap");
        }

        static ini_config build_ini(const parsed_options& opts)
        {
            ini_config ini;
            auto bundled = sdl::resolve_bundled_asset("osect.ini");
            if(!bundled.empty())
            {
                ini.merge(ini_config(bundled));
            }
            auto user = sdl::resolve_user_asset("osect.ini");
            if(!user.empty())
            {
                ini.merge(ini_config(user));
            }
            if(opts.conf_path)
            {
                ini.merge(ini_config(*opts.conf_path));
            }
            return ini;
        }

        static const char* resolve_gpu_driver(const parsed_options& opts)
        {
#ifndef OSECT_HAVE_DXIL
            if(opts.gpu_driver && *opts.gpu_driver == "direct3d12")
            {
                throw std::runtime_error(
                    "--gpu direct3d12 not available: this build was configured without D3D12 support."
                    " Rebuild with -DOSECT_ENABLE_D3D12=ON and dxc on PATH (or in $VULKAN_SDK/bin).");
            }
#endif
            return opts.gpu_driver ? opts.gpu_driver->c_str() : "vulkan";
        }

        impl(const std::vector<std::string>& cmdline)
            : opts(parse_cmdline(cmdline)),
              db_path(resolve_db_path(opts, cmdline.empty() ? std::string{"osect"} : cmdline[0])),
              tile_path(resolve_tile_path(opts)),
              sdl_ctx(opts.verbosity),
              win(sdl_ctx, "OpenSectional", 1280, 1024,
                  sdl::window_flags::resizable | sdl::window_flags::high_pixel_density),
              dev(win, true, resolve_gpu_driver(opts)),
              imgui_ctx(dev, win),
              eph(opts.offline),
              ini(build_ini(opts)),
              map(dev, tile_path.empty() ? nullptr : tile_path.c_str(), db_path.c_str(), ini, eph, 1280, 1024),
              planner_db(db_path.c_str()),
              planner(planner_db),
              submitter(planner),
              plan_options(load_route_plan_options(ini)),
              static_sources(planner_db.list_data_sources())
        {
            event_mgr.set_raw_event_hook([this](const void* event) { imgui_ctx.process_event(event); });
            event_mgr.add_listener(map.event_listener());

            ui.set_route_planner_defaults(plan_options.max_leg_length_nm, plan_options.use_airways);
        }

        std::vector<data_source> build_data_sources()
        {
            auto eph_sources = eph.as_data_sources();
            std::vector<data_source> merged;
            merged.reserve(static_sources.size() + eph_sources.size());
            for(const auto& s : static_sources)
            {
                // The legacy static "tfr" META row is replaced by the
                // ephemeral source.
                if(s.name == "tfr")
                {
                    continue;
                }
                merged.push_back(s);
            }
            for(auto& s : eph_sources)
            {
                merged.push_back(std::move(s));
            }
            return merged;
        }

        bool handle_visibility(const ui_overlay_result& r)
        {
            if(!r.visibility_changed)
            {
                return false;
            }
            map.set_visibility(ui.visibility());
            return true;
        }

        bool handle_search_selection(const ui_overlay_result& r)
        {
            if(!r.selected_hit_index)
            {
                return false;
            }
            auto idx = *r.selected_hit_index;
            if(idx >= 0 && idx < static_cast<int>(ui.search_results().size()))
            {
                map.focus_on_hit(ui.search_results()[idx]);
            }
            ui.set_search_results({});
            return true;
        }

        bool handle_search_query(const ui_overlay_result& r)
        {
            if(!r.search_query)
            {
                return false;
            }
            const auto& q = *r.search_query;
            ui.set_search_results(q.empty() ? std::vector<search_hit>{} : map.search(q, SEARCH_RESULT_LIMIT));
            return true;
        }

        bool handle_route_request(const ui_overlay_result& r)
        {
            if(!r.requested_route_text)
            {
                return false;
            }
            if(r.requested_route_text->empty())
            {
                map.clear_route();
                ui.clear_route_state();
            }
            else
            {
                // Snapshot the GUI knobs into the planner options
                // for this submission. ini-driven preferences are
                // already in `plan_options`; we just overlay
                // max_leg and use-airways from the UI.
                auto opts = plan_options;
                opts.max_leg_length_nm = r.route_max_leg_nm;
                opts.use_airways = r.route_use_airways;
                submitter.submit(*r.requested_route_text, opts);
            }
            return true;
        }

        // Single per-frame read of the submitter's state. poll()
        // guarantees `pending` and `completion` are mutually
        // exclusive, so a finished plan never arrives in the same
        // frame that the spinner is shown. Must follow
        // handle_route_request in the per-frame chain so a freshly
        // submitted worker is observed as `pending` in the same
        // frame, keeping the planning flag continuous.
        bool handle_route_status()
        {
            auto status = submitter.poll();
            ui.set_route_planning(status.pending);
            auto dirty = status.pending;
            if(status.completion)
            {
                if(status.completion->route)
                {
                    ui.set_route_state(*status.completion->route);
                    map.set_route(std::move(*status.completion->route));
                }
                else
                {
                    ui.clear_route_state(status.completion->error);
                }
                dirty = true;
            }
            return dirty;
        }

        bool handle_route_dirty()
        {
            if(!map.drain_route_dirty())
            {
                return false;
            }
            if(map.route())
            {
                ui.set_route_state(*map.route());
            }
            else
            {
                ui.clear_route_state();
            }
            return true;
        }

        void run()
        {
            auto needs_render = true;
            auto last_render_ms = 0.0F;
            while(true)
            {
                map.set_imgui_wants_mouse(imgui_ctx.wants_mouse());
                map.set_imgui_wants_keyboard(imgui_ctx.wants_keyboard());

                if(event_mgr.dispatch_events())
                {
                    break;
                }

                needs_render = map.update();

                imgui_ctx.new_frame();

                // Refresh data-status state for the panel each frame so
                // ephemeral sources' freshness flips appear without an
                // extra signal path.
                ui.set_data_sources(build_data_sources());

                auto ui_result = ui.draw(last_render_ms, map.feature_types());
                needs_render |= handle_visibility(ui_result);
                needs_render |= handle_search_selection(ui_result);
                needs_render |= handle_search_query(ui_result);
                needs_render |= handle_route_request(ui_result);
                needs_render |= handle_route_status();
                needs_render |= handle_route_dirty();
                needs_render |= map.draw_imgui();
                needs_render |= imgui_ctx.wants_mouse();
                needs_render |= imgui_ctx.warming_up();

                imgui_ctx.end_frame();

                if(needs_render)
                {
                    sdl::timer render_timer;
                    sdl::command_buffer cmd(dev);

                    unsigned width = 0;
                    unsigned height = 0;
                    auto swapchain = cmd.acquire_swapchain(win, width, height);
                    if(swapchain)
                    {
                        map.render_frame(cmd, *swapchain);
                        imgui_ctx.render(cmd, *swapchain);
                    }

                    last_render_ms = render_timer.elapsed_ms();
                }
            }
        }
    };

    program::program(const std::vector<std::string>& cmdline) : pimpl(std::make_unique<impl>(cmdline))
    {
    }

    program::~program() = default;

    void program::run()
    {
        pimpl->run();
    }

    void wake_main_thread()
    {
        sdl::event_manager::push_user_event();
    }
}
