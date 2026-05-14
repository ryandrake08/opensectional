#include "program.hpp"
#include "ephemeral_database.hpp"
#include "ephemeral_source.hpp"
#include "feature_type.hpp"
#include "flight_route.hpp"
#include "ini_config.hpp"
#include "map_widget.hpp"
#include "nasr_database.hpp"
#include "route_plan_config.hpp"
#include "route_submitter.hpp"
#include "tfr_refresher.hpp"
#include "ui_overlay.hpp"
#include "user_database.hpp"
#include <imgui/context.hpp>
#include <cstdint>
#include <iostream>
#include <optional>
#include <sdl/command_buffer.hpp>
#include <sdl/copy_pass.hpp>
#include <sdl/device.hpp>
#include <sdl/event.hpp>
#include <sdl/filesystem.hpp>
#include <sdl/instance.hpp>
#include <sdl/log.hpp>
#include <sdl/render_pass.hpp>
#include <sdl/texture.hpp>
#include <sdl/timer.hpp>
#include <sdl/window.hpp>
#include <stdexcept>
#include <unordered_map>

namespace osect
{
    namespace
    {
        constexpr auto SEARCH_RESULT_LIMIT = 12;

        // SDL event type used purely to wake SDL_WaitEvent — no
        // payload, no handler. Background producers push it through
        // wake_main_thread(); the main loop's render path also
        // pushes it to keep the imgui warmup frames flowing without
        // needing user input. Allocated once via SDL_RegisterEvents
        // on first call; thread-safe via function-local static init.
        std::uint32_t wake_event_type()
        {
            static const std::uint32_t type = sdl::event_manager::register_event_type();
            return type;
        }

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
            bool vsync = false;
            bool gpu_debug = false;
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
                else if(arg == "--vsync")
                {
                    opts.vsync = true;
                }
                else if(arg == "--gpu_debug")
                {
                    opts.gpu_debug = true;
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
        // Null in --offline mode. When present, supplies the UPD
        // indicator via is_refreshing().
        std::unique_ptr<tfr_refresher> tfrs;
        ini_config ini;
        map_widget map;
        route_submitter submitter;
        route_plan_options plan_options;
        ui_overlay ui;
        // Persistent saved-route store. Source of truth for route
        // text and identity; in-memory map.routes() is a cache.
        user_database udb;
        // Snapshot of the last visibility state we saw, kept so
        // handle_visibility can log the diff each time the user
        // toggles a layer / altitude band / chart type.
        layer_visibility prev_vis;
        // Maps each route-panel tab id to its planned route's
        // persistent route_id. Tabs without a planned route are
        // absent. route_id is stable across mutations (no
        // shift-after-remove housekeeping needed).
        std::unordered_map<std::uint64_t, route_id> tab_to_route;
        // Last active panel tab id observed via active_tab_changed.
        // Used to decide whether a freshly-planned route should pull
        // the view (only when the user is still focused on the
        // submitting tab when the result arrives).
        std::uint64_t active_tab_id = 0;

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
                sdl::log_info("ini merge: bundled " + bundled);
                ini.merge(ini_config(bundled));
            }
            auto user = sdl::resolve_user_asset("osect.ini");
            if(!user.empty())
            {
                sdl::log_info("ini merge: user " + user);
                ini.merge(ini_config(user));
            }
            if(opts.conf_path)
            {
                sdl::log_info("ini merge: --conf " + *opts.conf_path);
                ini.merge(ini_config(*opts.conf_path));
            }
            return ini;
        }

        static const char* resolve_gpu_driver(const parsed_options& opts)
        {
            // Default to Vulkan for cross-platform parity. Returning nullptr
            // here would let SDL auto-pick (Metal on macOS, D3D12 on Windows
            // when DXIL is built, Vulkan on Linux)
            if(!opts.gpu_driver)
            {
                return "vulkan";
            }
            const std::string& d = *opts.gpu_driver;
            if(d == "vulkan")
            {
                return d.c_str();
            }
            if(d == "metal")
            {
#ifdef __APPLE__
                return d.c_str();
#else
                throw std::runtime_error("--gpu metal not available: Metal is supported on macOS only.");
#endif
            }
            if(d == "direct3d12")
            {
#ifdef OSECT_HAVE_DXIL
                return d.c_str();
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

        impl(const std::vector<std::string>& cmdline)
            : opts(parse_cmdline(cmdline)),
              db_path(resolve_db_path(opts, cmdline.empty() ? std::string{"osect"} : cmdline[0])),
              tile_path(resolve_tile_path(opts)),
              sdl_ctx(opts.verbosity),
              win(sdl_ctx, "OpenSectional", 1280, 1024,
                  sdl::window_flags::resizable | sdl::window_flags::high_pixel_density),
              dev(win, resolve_gpu_driver(opts), opts.vsync, opts.gpu_debug),
              imgui_ctx(dev, win),
              tfrs(opts.offline ? nullptr : std::make_unique<tfr_refresher>(ephemeral_database::default_path())),
              ini(build_ini(opts)),
              map(dev, tile_path.empty() ? nullptr : tile_path.c_str(), db_path.c_str(), ini, 1280, 1024),
              submitter(db_path.c_str()),
              plan_options(load_route_plan_options(ini)),
              udb(user_database::default_path()),
              prev_vis(ui.visibility())
        {
            event_mgr.set_raw_event_hook([this](const void* event) { imgui_ctx.process_event(event); });
            event_mgr.add_listener(map.event_listener());
            event_mgr.set_event_handler(ephemeral_refresh_event_type(),
                                        [this](int code) { on_ephemeral_refresh(code); });

            push_data_sources();

            ui.set_route_planner_defaults(plan_options.max_leg_length_nm, plan_options.use_airways);

            // Read every persisted route from user.db and seed
            // map_widget + ui_overlay so the user sees the same tabs
            // they had last session. A row whose stored waypoints are
            // structurally invalid is logged and skipped; it stays in
            // user.db for a future build to handle.
            const auto records = udb.load_routes();
            if(records.empty())
            {
                return;
            }
            std::size_t loaded = 0;
            std::size_t skipped = 0;
            for(const auto& rec : records)
            {
                try
                {
                    flight_route route(rec.waypoints);
                    auto tab_id = ui.add_route_tab(route);
                    map.add_route(rec.route_id);
                    tab_to_route.emplace(tab_id, rec.route_id);
                    ++loaded;
                }
                catch(const std::exception& e)
                {
                    sdl::log_warn("user.db: route_id=" + std::to_string(rec.route_id) +
                                  " failed to load: " + e.what() + " — row retained, not loaded");
                    ++skipped;
                }
            }
            sdl::log_info("user.db: restored " + std::to_string(loaded) + " routes (" +
                          std::to_string(skipped) + " skipped)");

            // Log info about the GPU driver
            const char* gpu = resolve_gpu_driver(opts);
            sdl::log_info(std::string("started: gpu=") + (gpu ? gpu : "auto") + " db=" + db_path +
                          " basemap=" + (tile_path.empty() ? std::string("(none)") : tile_path) +
                          " offline=" + (opts.offline ? "true" : "false"));
        }

        // Read both databases, merge, push to the UI. Fires on
        // events and at startup, not per frame, so opening fresh
        // connections each call is fine.
        void push_data_sources()
        {
            auto merged = nasr_database(db_path.c_str()).list_data_sources();
            auto eph = ephemeral_database(ephemeral_database::default_path()).list_data_sources();
            if(tfrs)
            {
                const bool updating = tfrs->is_refreshing();
                for(auto& s : eph)
                {
                    if(s.name == "tfr")
                    {
                        s.updating = updating;
                        break;
                    }
                }
            }
            merged.insert(merged.end(), std::make_move_iterator(eph.begin()),
                          std::make_move_iterator(eph.end()));
            ui.set_data_sources(std::move(merged));
        }

        // Fires on every refresh transition (start, success, end).
        // push_data_sources picks up the new timestamp on success;
        // start/end fires re-read SOURCE_META no-op-style.
        // map.on_ephemeral_refresh invalidates the feature build —
        // idempotent, so duplicate fires collapse to one rebuild.
        void on_ephemeral_refresh(int code)
        {
            push_data_sources();
            map.on_ephemeral_refresh(static_cast<ephemeral_source>(code));
        }

        // Find the human label for a layer enum value: "Basemap" for
        // layer_basemap, otherwise the feature_type's UI label.
        std::string layer_label(int layer_id) const
        {
            if(layer_id == layer_basemap)
            {
                return "Basemap";
            }
            for(const auto& t : map.feature_types())
            {
                if(t->layer_id() == layer_id)
                {
                    return t->label();
                }
            }
            return "layer#" + std::to_string(layer_id);
        }

        static const char* altitude_band_name(const altitude_filter& a)
        {
            if(a.show_low)
            {
                return "low";
            }
            if(a.show_high)
            {
                return "high";
            }
            if(a.show_unlimited)
            {
                return "unlimited";
            }
            return "(none)";
        }

        static const char* chart_name(chart_type c)
        {
            switch(c)
            {
            case chart_type::sectional:
                return "sectional";
            case chart_type::ifr_low:
                return "ifr_low";
            case chart_type::ifr_high:
                return "ifr_high";
            }
            return "(unknown)";
        }

        bool handle_visibility(const ui_overlay_result& r)
        {
            if(!r.visibility_changed)
            {
                return false;
            }
            const auto& now = ui.visibility();
            for(int i = 0; i < layer_count; ++i)
            {
                if(prev_vis[i] != now[i])
                {
                    sdl::log_info("visibility: " + layer_label(i) + " = " + (now[i] ? "on" : "off"));
                }
            }
            if(prev_vis.altitude != now.altitude)
            {
                sdl::log_info(std::string("altitude band: ") + altitude_band_name(now.altitude));
            }
            if(prev_vis.chart != now.chart)
            {
                sdl::log_info(std::string("chart: ") + chart_name(now.chart));
            }
            prev_vis = now;
            map.set_visibility(now);
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
                const auto& hit = ui.search_results()[idx];
                sdl::log_info("search selection: " + hit.entity_type + " \"" + hit.ids + "\" \"" + hit.name + "\"");
                map.focus_on_hit(hit);
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

        // Reverse lookup: which tab id (if any) owns route `rid`.
        // O(n) over tab_to_route; n is the number of tabs with
        // planned routes (typically <10).
        std::optional<std::uint64_t> tab_for_route(route_id rid) const
        {
            for(const auto& [tab_id, id] : tab_to_route)
            {
                if(id == rid)
                {
                    return tab_id;
                }
            }
            return std::nullopt;
        }

        bool handle_route_request(const ui_overlay_result& r)
        {
            if(!r.route_submit)
            {
                return false;
            }
            const auto& req = *r.route_submit;

            // Empty text is the Clear-button signal: drop the tab's
            // route (if it had one) but keep the tab itself.
            if(req.text.empty())
            {
                auto it = tab_to_route.find(req.tab_id);
                if(it != tab_to_route.end())
                {
                    auto rid = it->second;
                    sdl::log_info("route cleared: tab=" + std::to_string(req.tab_id));
                    map.remove_route(rid);
                    tab_to_route.erase(it);
                    try
                    {
                        udb.delete_route(rid);
                    }
                    catch(const std::exception& e)
                    {
                        sdl::log_warn(std::string("user.db: delete_route failed (continuing): ") + e.what());
                    }
                }
                ui.clear_route_state(req.tab_id);
                return true;
            }

            // Snapshot the GUI knobs into the planner options for
            // this submission. ini-driven preferences are already in
            // plan_options; we just overlay max_leg and use-airways
            // from the panel that submitted.
            auto opts = plan_options;
            opts.max_leg_length_nm = req.max_leg_nm;
            opts.use_airways = req.use_airways;
            if(auto err = validate_route_plan_options(opts); !err.empty())
            {
                sdl::log_warn("route submit rejected: " + err);
                ui.clear_route_state(req.tab_id, err);
                return true;
            }
            sdl::log_info("route submit: tab=" + std::to_string(req.tab_id) + " \"" + req.text +
                          "\" (max_leg=" + std::to_string(static_cast<int>(opts.max_leg_length_nm)) +
                          "nm airways=" + (opts.use_airways ? "true" : "false") + ")");
            submitter.submit(req.text, opts, req.tab_id);
            ui.set_route_planning(req.tab_id, true);
            return true;
        }

        // Single per-frame read of the submitter's state. poll()
        // guarantees `pending` and `completion` are mutually
        // exclusive, so a finished plan never arrives in the same
        // frame that the spinner is shown.
        bool handle_route_status()
        {
            auto status = submitter.poll();
            if(!status.completion)
            {
                return status.pending;
            }
            auto tag = status.completion->tag;
            ui.set_route_planning(tag, false);
            if(!ui.has_tab(tag))
            {
                // Originating tab was closed before the plan
                // completed. Drop the result.
                sdl::log_info("route plan dropped: tab=" + std::to_string(tag) + " no longer present");
                return true;
            }
            if(status.completion->route)
            {
                auto& route = *status.completion->route;
                sdl::log_info("route planned: tab=" + std::to_string(tag) + " " +
                              std::to_string(route.waypoints.size()) + " waypoints, " +
                              std::to_string(static_cast<int>(route.total_distance_nm())) + " nm");
                ui.set_route_state(tag, route);

                auto it = tab_to_route.find(tag);
                route_id rid = 0;
                if(it != tab_to_route.end())
                {
                    rid = it->second;
                    try
                    {
                        udb.update_route(rid, route.to_rows());
                    }
                    catch(const std::exception& e)
                    {
                        sdl::log_warn(std::string("user.db: update_route failed (continuing): ") + e.what());
                    }
                    map.replace_route(rid);
                }
                else
                {
                    try
                    {
                        rid = udb.insert_route(route.to_rows());
                    }
                    catch(const std::exception& e)
                    {
                        sdl::log_warn(std::string("user.db: insert_route failed, dropping plan: ") + e.what());
                        ui.clear_route_state(tag, "Failed to save route");
                        return true;
                    }
                    map.add_route(rid);
                    tab_to_route.emplace(tag, rid);
                }

                // Only pull the view, activate, and highlight if the
                // submitting tab is still the user's focused tab —
                // otherwise the map stays where the user has it.
                if(tag == active_tab_id)
                {
                    map.set_active_route(rid);
                    map.select_route(rid);
                    map.fit_view_to_route(rid);
                }
            }
            else
            {
                sdl::log_info("route plan failed: tab=" + std::to_string(tag) + " " + status.completion->error);
                ui.clear_route_state(tag, status.completion->error);
            }
            return true;
        }

        bool handle_route_dirty()
        {
            auto result = map.drain_route_drag_result();
            if(!result)
            {
                return false;
            }
            auto active = map.active_route();
            if(!active)
            {
                return true;
            }
            try
            {
                udb.update_route(*active, result->to_rows());
            }
            catch(const std::exception& e)
            {
                // On write failure, user.db keeps the prior text;
                // the next feature build will render the old route.
                // Leave the tab's text alone too so disk and UI agree.
                sdl::log_warn(std::string("user.db: drag update_route failed: ") + e.what());
                return true;
            }
            auto tab = tab_for_route(*active);
            if(!tab)
            {
                return true;
            }
            ui.set_route_state(*tab, *result);
            return true;
        }

        bool handle_active_tab_changed(const ui_overlay_result& r)
        {
            if(!r.active_tab_changed)
            {
                return false;
            }
            active_tab_id = *r.active_tab_changed;
            auto it = tab_to_route.find(active_tab_id);
            auto rid = it != tab_to_route.end() ? std::optional<route_id>(it->second) : std::optional<route_id>{};
            map.set_active_route(rid);
            // Tab switch is the user's intentional focus gesture, so
            // align selection with the new tab's route. They can
            // diverge again by clicking a different route on the map.
            map.select_route(rid);
            return true;
        }

        bool handle_tab_closed(const ui_overlay_result& r)
        {
            if(!r.tab_closed)
            {
                return false;
            }
            auto it = tab_to_route.find(*r.tab_closed);
            if(it != tab_to_route.end())
            {
                auto rid = it->second;
                sdl::log_info("route removed: tab=" + std::to_string(*r.tab_closed) +
                              " route_id=" + std::to_string(rid));
                map.remove_route(rid);
                tab_to_route.erase(it);
                try
                {
                    udb.delete_route(rid);
                }
                catch(const std::exception& e)
                {
                    sdl::log_warn(std::string("user.db: delete_route failed (continuing): ") + e.what());
                }
            }
            return true;
        }

        bool handle_route_delete_request()
        {
            auto rid = map.drain_route_delete_request();
            if(!rid)
            {
                return false;
            }
            auto tab = tab_for_route(*rid);
            if(tab)
            {
                sdl::log_info("route deleted via popup: tab=" + std::to_string(*tab) +
                              " route_id=" + std::to_string(*rid));
                ui.close_tab(*tab);
                tab_to_route.erase(*tab);
            }
            map.remove_route(*rid);
            try
            {
                udb.delete_route(*rid);
            }
            catch(const std::exception& e)
            {
                sdl::log_warn(std::string("user.db: delete_route failed (continuing): ") + e.what());
            }
            return true;
        }

        bool handle_route_activate_request()
        {
            auto rid = map.drain_route_activate_request();
            if(!rid)
            {
                return false;
            }
            auto tab = tab_for_route(*rid);
            if(!tab)
            {
                return false;
            }
            sdl::log_info("activate route via map click: tab=" + std::to_string(*tab) +
                          " route_id=" + std::to_string(*rid));
            ui.set_active_tab(*tab);
            active_tab_id = *tab;
            map.set_active_route(*rid);
            map.select_route(*rid);
            return true;
        }

        void run()
        {
            auto last_render_ms = 0.0F;
            // Carried across iterations: the state handlers run before
            // ui.draw() (see below), so they consume the ui_result
            // produced by the previous iteration's draw.
            ui_overlay_result ui_result;
            while(true)
            {
                map.set_imgui_wants_mouse(imgui_ctx.wants_mouse());
                map.set_imgui_wants_keyboard(imgui_ctx.wants_keyboard());

                if(event_mgr.dispatch_events())
                {
                    sdl::log_info("shutting down");
                    break;
                }

                // State handlers run before ui.draw() so their
                // mutations — active tab, popups, route tabs — land in
                // this frame's draw rather than a frame late. They
                // consume two inputs: state set by this iteration's
                // dispatch_events() (a map click activating a route,
                // drag results), which they see immediately; and the
                // previous frame's ui_result (tab clicks, the pick
                // selector), whose effects land one event later — the
                // mouse-up that follows every such interaction always
                // provides that iteration.
                bool needs_render = false;
                needs_render |= handle_visibility(ui_result);
                needs_render |= handle_search_selection(ui_result);
                needs_render |= handle_search_query(ui_result);
                needs_render |= handle_tab_closed(ui_result);
                needs_render |= handle_active_tab_changed(ui_result);
                needs_render |= handle_route_request(ui_result);
                needs_render |= handle_route_status();
                needs_render |= handle_route_dirty();
                needs_render |= handle_route_delete_request();
                needs_render |= handle_route_activate_request();

                // Draw all UI, producing the ui_result the next
                // iteration's handlers consume.
                imgui_ctx.new_frame();
                ui_result = ui.draw(last_render_ms, map.feature_types());
                needs_render |= map.draw_imgui();
                imgui_ctx.end_frame();

                // Drain async results that arrived during the wait,
                // and submit any new build requests this frame's
                // mutations triggered. Single per-frame sync point.
                needs_render |= map.update();
                needs_render |= imgui_ctx.wants_mouse();

                // HACK: Handle imgui "warmup" by scheduling two runs
                // through the event loop on startup. Need to figure
                // out a better way to do this.
                if(imgui_ctx.warming_up())
                {
                    needs_render = true;
                    // Schedule the next loop iteration so the
                    // remaining warmup frames flow without waiting
                    // on user input. imgui's warmup_pending() tells
                    // us whether more frames are still queued; the
                    // wake is harmless if none are.
                    if(imgui_ctx.warmup_pending())
                    {
                        sdl::event_manager::push_event(wake_event_type(), 0);
                    }
                }

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
        sdl::event_manager::push_event(wake_event_type(), 0);
    }
}
