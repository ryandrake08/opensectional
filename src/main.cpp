#include "ephemeral_data.hpp"
#include "ini_config.hpp"
#include "map_widget.hpp"
#include "nasr_database.hpp"
#include "route_plan_config.hpp"
#include "route_planner.hpp"
#include "route_submitter.hpp"
#include "ui_overlay.hpp"
#include <cstring>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <imgui/context.hpp>
#include <iostream>
#include <sdl/command_buffer.hpp>
#include <sdl/copy_pass.hpp>
#include <sdl/device.hpp>
#include <sdl/render_pass.hpp>
#include <sdl/texture.hpp>
#include <sdl/event.hpp>
#include <sdl/filesystem.hpp>
#include <sdl/instance.hpp>
#include <sdl/timer.hpp>
#include <sdl/window.hpp>

namespace
{
    void signal_handler(int /*signum*/)
    {
        sdl::event_manager::push_quit_event();
    }

    void print_usage(std::ostream& out, const char* prog)
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
}

int main(int argc, char** argv)
{
    auto verbosity = 0;
    const char* gpu_driver = nullptr;
    const char* tile_path = nullptr;
    const char* db_path = nullptr;
    const char* conf_path = nullptr;
    bool offline = false;
    auto argi = 1;
    while(argi < argc && argv[argi][0] == '-')
    {
        if(std::strcmp(argv[argi], "-h") == 0 || std::strcmp(argv[argi], "--help") == 0)
        {
            print_usage(std::cout, argv[0]);
            return EXIT_SUCCESS;
        }
        else if(argv[argi][1] == 'v' && argv[argi][2] != '-')
        {
            const char* p = argv[argi] + 1;
            while(*p == 'v') { verbosity++; p++; }
            if(*p != '\0')
            {
                std::cerr << "Unknown option: " << argv[argi] << std::endl;
                return EXIT_FAILURE;
            }
        }
        else if((std::strcmp(argv[argi], "-g") == 0 || std::strcmp(argv[argi], "--gpu") == 0) && argi + 1 < argc)
            gpu_driver = argv[++argi];
        else if((std::strcmp(argv[argi], "-b") == 0 || std::strcmp(argv[argi], "--basemap") == 0) && argi + 1 < argc)
            tile_path = argv[++argi];
        else if((std::strcmp(argv[argi], "-d") == 0 || std::strcmp(argv[argi], "--database") == 0) && argi + 1 < argc)
            db_path = argv[++argi];
        else if((std::strcmp(argv[argi], "-c") == 0 || std::strcmp(argv[argi], "--conf") == 0) && argi + 1 < argc)
            conf_path = argv[++argi];
        else if(std::strcmp(argv[argi], "--offline") == 0)
            offline = true;
        else
        {
            std::cerr << "Unknown option: " << argv[argi] << std::endl;
            return EXIT_FAILURE;
        }
        argi++;
    }
    if(argi < argc)
    {
        std::cerr << "Unexpected positional argument: " << argv[argi] << std::endl;
        return EXIT_FAILURE;
    }

#ifndef OSECT_HAVE_DXIL
    if(gpu_driver && std::strcmp(gpu_driver, "direct3d12") == 0)
    {
        std::cerr << "--gpu direct3d12 not available: this build was configured without D3D12 support."
                  << " Rebuild with -DOSECT_ENABLE_D3D12=ON and dxc on PATH (or in $VULKAN_SDK/bin)." << std::endl;
        return EXIT_FAILURE;
    }
#endif

    // Resolve bundled assets for any paths not provided on the command line.
    std::string db_path_owned;
    if(!db_path)
    {
        db_path_owned = sdl::resolve_bundled_asset("osect.db");
        if(db_path_owned.empty())
        {
            std::cerr << "No osect.db supplied and none found next to the executable or in the current directory." << std::endl;
            print_usage(std::cerr, argv[0]);
            return EXIT_FAILURE;
        }
        db_path = db_path_owned.c_str();
    }
    std::string tile_path_owned;
    if(!tile_path)
    {
        tile_path_owned = sdl::resolve_bundled_asset("basemap");
        if(!tile_path_owned.empty()) tile_path = tile_path_owned.c_str();
    }

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
#if defined(SIGPIPE)
    std::signal(SIGPIPE, SIG_IGN);
#endif

    try
    {
        sdl::instance sdl_ctx(verbosity);
        auto win_flags = sdl::window_flags::resizable | sdl::window_flags::high_pixel_density;
        sdl::window win(sdl_ctx, "OpenSectional", 1280, 1024, win_flags);

        const char* default_driver = "vulkan";
        sdl::device dev(win, true, gpu_driver ? gpu_driver : default_driver);

        imgui::context imgui_ctx(dev, win);

        sdl::event_manager event_mgr;
        event_mgr.set_raw_event_hook([&imgui_ctx](const void* event) {
            imgui_ctx.process_event(event);
        });

        // Ephemeral data facade: owns the shared HTTP client, the
        // on-disk cache, and every per-source module that consumes
        // them. Constructed before map_widget because map_widget's
        // render/pick paths read ephemeral data through it rather
        // than from osect.db.
        osect::ephemeral_data eph(offline);

        // Build the cascaded ini override layer. Each layer is optional;
        // a missing file is silently skipped. Layered last-wins so the
        // explicit -c/--conf path takes precedence over user, which
        // takes precedence over the bundled ini.
        ini_config ini;
        auto bundled = sdl::resolve_bundled_asset("osect.ini");
        if(!bundled.empty()) ini.merge(ini_config(bundled));
        auto user = sdl::resolve_user_asset("osect.ini");
        if(!user.empty()) ini.merge(ini_config(user));
        if(conf_path) ini.merge(ini_config(conf_path));

        map_widget map(dev, tile_path, db_path, ini, eph, 1280, 1024);
        event_mgr.add_listener(map.event_listener());

        // Sigil expansion runs on its own database/planner instance
        // so map_widget can stay focused on rendering. SQLite opens
        // are cheap and read-only.
        osect::nasr_database planner_db(db_path);
        osect::route_planner planner(planner_db);
        osect::route_submitter submitter(planner);

        // Route-planner preferences come from the same ini; GUI
        // overrides max_leg and the use-airways toggle per submission.
        auto plan_options = osect::load_route_plan_options(ini);

        osect::ui_overlay ui;
        ui.set_route_planner_defaults(plan_options.max_leg_length_nm,
                                       plan_options.use_airways);

        // Static-source META rows are read once at startup; the
        // ephemeral source rows get pushed every frame so freshness
        // and "refreshing…" indicators stay live.
        const auto static_sources = planner_db.list_data_sources();

        const auto build_data_sources = [&]()
        {
            auto eph_sources = eph.as_data_sources();
            std::vector<osect::data_source> merged;
            merged.reserve(static_sources.size() + eph_sources.size());
            for(const auto& s : static_sources)
            {
                // Filter out the legacy static "tfr" META row — the
                // ephemeral source replaces it.
                if(s.name == "tfr") continue;
                merged.push_back(s);
            }
            for(auto& s : eph_sources)
                merged.push_back(std::move(s));
            return merged;
        };

        std::string last_search_query;
        constexpr auto SEARCH_RESULT_LIMIT = 12;

        auto needs_render = true;
        auto last_render_ms = 0.0F;
        while(true)
        {
            map.set_imgui_wants_mouse(imgui_ctx.wants_mouse());
            map.set_imgui_wants_keyboard(imgui_ctx.wants_keyboard());

            if(event_mgr.dispatch_events()) break;

            needs_render = map.update();

            imgui_ctx.new_frame();

            // Refresh data-status state for the panel each frame so
            // ephemeral sources' freshness flips appear without an
            // extra signal path.
            ui.set_data_sources(build_data_sources());

            auto ui_result = ui.draw(last_render_ms, map.feature_types());
            if(ui_result.visibility_changed)
            {
                map.set_visibility(ui.visibility());
                needs_render = true;
            }
            if(ui_result.selected_hit_index)
            {
                auto idx = *ui_result.selected_hit_index;
                if(idx >= 0 && idx < static_cast<int>(ui.search_results().size()))
                    map.focus_on_hit(ui.search_results()[idx]);
                ui.set_search_results({});
                last_search_query.clear();
                needs_render = true;
            }
            else if(ui_result.search_query != last_search_query)
            {
                last_search_query = ui_result.search_query;
                ui.set_search_results(
                    last_search_query.empty()
                        ? std::vector<osect::search_hit>{}
                        : map.search(last_search_query, SEARCH_RESULT_LIMIT));
                needs_render = true;
            }

            if(ui_result.clear_route)
            {
                map.clear_route();
                ui.clear_route_state();
                needs_render = true;
            }
            else if(ui_result.submit_route_text)
            {
                // Snapshot the GUI knobs into the planner options
                // for this submission. ini-driven preferences are
                // already in `plan_options`; we just overlay
                // max_leg and use-airways from the UI.
                auto opts = plan_options;
                opts.max_leg_length_nm = ui_result.route_max_leg_nm;
                opts.use_airways = ui_result.route_use_airways;
                submitter.submit(*ui_result.submit_route_text, opts);
                needs_render = true;
            }

            // Keep frames flowing while an async plan is in progress
            // so the UI can animate its indicator.
            auto plan_pending = submitter.pending();
            ui.set_route_planning(plan_pending);
            if(plan_pending) needs_render = true;

            try
            {
                if(auto expanded = submitter.drain())
                {
                    if(expanded->empty())
                    {
                        map.clear_route();
                        ui.clear_route_state();
                    }
                    else
                    {
                        map.set_route_text(*expanded);
                        if(map.route()) ui.set_route_state(*map.route());
                        else ui.clear_route_state();
                    }
                    needs_render = true;
                }
            }
            catch(const osect::route_parse_error& e)
            {
                ui.clear_route_state(e.what());
                needs_render = true;
            }

            if(map.drain_route_dirty())
            {
                if(map.route()) ui.set_route_state(*map.route());
                else ui.clear_route_state();
                needs_render = true;
            }

            if(map.draw_imgui()) needs_render = true;
            if(imgui_ctx.wants_mouse()) needs_render = true;
            if(imgui_ctx.warming_up()) needs_render = true;

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

        return EXIT_SUCCESS;
    }
    catch(const std::exception& e)
    {
        std::cerr << "FATAL ERROR: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }
}
