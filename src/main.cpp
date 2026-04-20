#include "map_widget.hpp"
#include "ui_overlay.hpp"
#include <cstring>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <imgui/context.hpp>
#include <iostream>
#include <memory>
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
    void signal_handler(int)
    {
        sdl::event_manager::push_quit_event();
    }

    void print_usage(std::ostream& out, const char* prog)
    {
        out << "Usage: " << prog << " [options]\n"
            << "  -h, --help                 Show this help and exit\n"
            << "  -v, -vv, -vvv              Increase verbosity\n"
            << "  -g, --gpu <driver>         GPU driver: vulkan, metal, direct3d12\n"
            << "  -b, --basemap <path>       Basemap tile directory\n"
            << "  -d, --database <nasr.db>   NASR SQLite database\n"
            << "  -c, --conf <nasrbrowse.ini> Chart style config\n"
            << "\n"
            << "When a path option is omitted, the asset is loaded from next to\n"
            << "the executable (installer layout) or the current directory.\n";
    }
}

int main(int argc, char** argv)
{
    int verbosity = 0;
    const char* gpu_driver = nullptr;
    const char* tile_path = nullptr;
    const char* db_path = nullptr;
    const char* conf_path = nullptr;
    int argi = 1;
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

    // Resolve bundled assets for any paths not provided on the command line.
    std::string db_path_owned;
    if(!db_path)
    {
        db_path_owned = sdl::resolve_bundled_asset("nasr.db");
        if(db_path_owned.empty())
        {
            std::cerr << "No nasr.db supplied and none found next to the executable or in the current directory." << std::endl;
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
    std::string ini_path_owned;
    if(!conf_path)
    {
        ini_path_owned = sdl::resolve_bundled_asset("nasrbrowse.ini");
        conf_path = ini_path_owned.empty() ? "nasrbrowse.ini" : ini_path_owned.c_str();
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
        sdl::window win(sdl_ctx, "NASRBrowse", 1280, 1024, win_flags);

        const char* default_driver = "vulkan";
        sdl::device dev(win, true, gpu_driver ? gpu_driver : default_driver);

        imgui::context imgui_ctx(dev, win);

        sdl::event_manager event_mgr;
        event_mgr.set_raw_event_hook([&imgui_ctx](const void* event) {
            imgui_ctx.process_event(event);
        });

        auto map = std::make_shared<map_widget>(dev, tile_path, db_path, conf_path);
        event_mgr.add_listener(map);
        map->framebuffer_size_event(1280, 1024);

        nasrbrowse::ui_overlay ui;
        std::string last_search_query;
        constexpr int SEARCH_RESULT_LIMIT = 12;

        bool needs_render = true;
        float last_render_ms = 0.0F;
        while(true)
        {
            map->set_imgui_wants_mouse(imgui_ctx.wants_mouse());
            map->set_imgui_wants_keyboard(imgui_ctx.wants_keyboard());

            if(event_mgr.dispatch_events()) break;

            needs_render = map->update();

            imgui_ctx.new_frame();

            auto ui_result = ui.draw(last_render_ms, map->zoom_level(),
                                      map->feature_types());
            if(ui_result.visibility_changed)
            {
                map->set_visibility(ui.visibility());
                needs_render = true;
            }
            if(ui_result.selected_hit_index)
            {
                int idx = *ui_result.selected_hit_index;
                if(idx >= 0 && idx < static_cast<int>(ui.search_results().size()))
                    map->focus_on_hit(ui.search_results()[idx]);
                ui.set_search_results({});
                last_search_query.clear();
                needs_render = true;
            }
            else if(ui_result.search_query != last_search_query)
            {
                last_search_query = ui_result.search_query;
                ui.set_search_results(
                    last_search_query.empty()
                        ? std::vector<nasrbrowse::search_hit>{}
                        : map->search(last_search_query, SEARCH_RESULT_LIMIT));
                needs_render = true;
            }

            if(ui_result.clear_route)
            {
                map->clear_route();
                ui.clear_route_state();
                needs_render = true;
            }
            else if(ui_result.submit_route_text)
            {
                try
                {
                    map->set_route_text(*ui_result.submit_route_text);
                    if(map->route())
                        ui.set_route_state(*map->route());
                    else
                        ui.clear_route_state();
                }
                catch(const nasrbrowse::route_parse_error& e)
                {
                    ui.clear_route_state(e.what());
                }
                needs_render = true;
            }

            if(map->drain_route_dirty())
            {
                if(map->route()) ui.set_route_state(*map->route());
                else ui.clear_route_state();
                needs_render = true;
            }

            if(map->draw_imgui()) needs_render = true;
            if(imgui_ctx.wants_mouse()) needs_render = true;
            if(imgui_ctx.warming_up()) needs_render = true;

            imgui_ctx.end_frame();

            if(needs_render)
            {
                sdl::timer render_timer;
                sdl::command_buffer cmd(dev);

                unsigned width, height;
                auto swapchain = cmd.acquire_swapchain(win, width, height);
                if(swapchain)
                {
                    map->render_frame(cmd, *swapchain);
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
