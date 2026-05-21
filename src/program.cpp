#include "program.hpp"
#include "app_options.hpp"
#include "ephemeral_database.hpp"
#include "ephemeral_source.hpp"
#include "feature_type.hpp"
#include "ini_config.hpp"
#include "map_widget.hpp"
#include "nasr_database.hpp"
#include "route_session.hpp"
#include "tfr_refresher.hpp"
#include "ui_overlay.hpp"
#include "user_database.hpp"
#include "waypoint_session.hpp"
#include <imgui/context.hpp>
#include <cstdint>
#include <optional>
#include <sdl/command_buffer.hpp>
#include <sdl/copy_pass.hpp>
#include <sdl/device.hpp>
#include <sdl/event.hpp>
#include <sdl/instance.hpp>
#include <sdl/log.hpp>
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

        // SDL event type used purely to wake SDL_WaitEvent — no
        // payload, no handler. Background producers push it through
        // wake_main_thread() after publishing a result so the render
        // loop notices without needing user input. Allocated once via
        // SDL_RegisterEvents on first call; thread-safe via
        // function-local static init.
        std::uint32_t wake_event_type()
        {
            static const std::uint32_t type = sdl::event_manager::register_event_type();
            return type;
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
        // Read-write user.db handle. waypoint_session and route_session
        // write user edits — created/renamed/deleted waypoints, saved
        // routes — through it; it is the source of truth for saved
        // routes and user waypoints.
        user_database udb;
        map_widget map;
        ui_overlay ui;
        // Applies user-waypoint create / rename / delete / drag edits
        // surfaced by map_widget, writing them through to user.db.
        waypoint_session waypoints;
        // Owns the route-panel-tab / map-route / user.db-row
        // correspondence and the background route planner.
        route_session routes;
        // Snapshot of the last visibility state we saw, kept so
        // handle_visibility can log the diff each time the user
        // toggles a layer / altitude band / chart type.
        layer_visibility prev_vis;

        impl(const std::vector<std::string>& cmdline)
            : opts(parse_cmdline(cmdline)),
              db_path(resolve_db_path(opts, cmdline.empty() ? std::string{"osect"} : cmdline[0])),
              tile_path(resolve_tile_path(opts)),
              sdl_ctx(opts.verbosity),
              win(sdl_ctx, "OpenSectional", 1280, 1024,
                  sdl::window_flags::resizable | sdl::window_flags::high_pixel_density),
              dev(win, resolve_gpu_driver(opts).c_str(), opts.vsync, opts.gpu_debug),
              imgui_ctx(dev, win),
              tfrs(opts.offline ? nullptr : std::make_unique<tfr_refresher>(ephemeral_database::default_path())),
              ini(build_ini(opts)),
              udb(user_database::default_path()),
              map(dev, tile_path.empty() ? nullptr : tile_path.c_str(), db_path.c_str(), ini, 1280, 1024),
              waypoints(map, udb),
              routes(ui, map, udb, ini, db_path.c_str()),
              prev_vis(ui.visibility())
        {
            event_mgr.set_raw_event_hook([this](const void* event) { imgui_ctx.process_event(event); });
            event_mgr.add_listener(map.event_listener());
            event_mgr.set_event_handler(ephemeral_refresh_event_type(),
                                        [this](int code) { on_ephemeral_refresh(code); });

            push_data_sources();

            // Restore the routes persisted in user.db into map_widget
            // and the route panel.
            routes.restore_from_db();

            // Log info about the GPU driver
            sdl::log_info("started: gpu=" + resolve_gpu_driver(opts) + " db=" + db_path +
                          " basemap=" + (tile_path.empty() ? std::string("(none)") : tile_path) +
                          " offline=" + (opts.offline ? "true" : "false"));
        }

        // Read both databases, merge, push to the UI. Fires on events
        // and at startup, not per frame, so opening a fresh connection
        // to each database per call is fine.
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

        // One pass of input-capture handoff, state handling, UI draw,
        // and conditional GPU render. ui_result and last_render_ms are
        // carried across calls. force renders unconditionally even when
        // nothing changed — used for the startup warmup passes, where
        // the render exists purely to let ImGui's auto-resize panels
        // settle their layout.
        void render_iteration(ui_overlay_result& ui_result, float& last_render_ms, bool force)
        {
            // Start the frame timer
            sdl::timer render_timer;

            map.set_imgui_wants_mouse(imgui_ctx.wants_mouse());
            map.set_imgui_wants_keyboard(imgui_ctx.wants_keyboard());

            // State handlers run before ui.draw() so their mutations —
            // active tab, popups, route tabs — land in this frame's
            // draw rather than a frame late. They consume two inputs:
            // state set by this iteration's dispatch_events() (a map
            // click activating a route, drag results), which they see
            // immediately; and the previous iteration's ui_result (tab
            // clicks, the pick selector), whose effects land one event
            // later — the mouse-up that follows every such interaction
            // always provides that iteration.
            bool needs_render = force;
            needs_render |= handle_visibility(ui_result);
            needs_render |= handle_search_selection(ui_result);
            needs_render |= handle_search_query(ui_result);
            needs_render |= routes.process(ui_result);
            needs_render |= waypoints.process();

            // Draw all UI, producing the ui_result the next iteration's
            // handlers consume.
            imgui_ctx.new_frame();
            ui_result = ui.draw(last_render_ms, map.feature_types());
            needs_render |= map.draw_imgui();
            imgui_ctx.end_frame();

            // Drain async results that arrived during the wait, and
            // submit any new build requests this frame's mutations
            // triggered. Single per-frame sync point.
            needs_render |= map.update();
            needs_render |= imgui_ctx.wants_mouse();

            if(needs_render)
            {
                sdl::command_buffer cmd(dev);

                auto swapchain = cmd.acquire_swapchain(win);
                if(swapchain)
                {
                    map.render_frame(cmd, *swapchain);
                    imgui_ctx.render(cmd, *swapchain);
                }

                last_render_ms = render_timer.elapsed_ms();
            }
        }

        void run()
        {
            auto last_render_ms = 0.0F;
            // Carried across iterations: the state handlers run before
            // ui.draw(), so they consume the ui_result produced by the
            // previous iteration's draw.
            ui_overlay_result ui_result;

            // ImGui's auto-resize panels need three draws to settle
            // their layout — the route panel's tab bar is the slowest
            // to converge. The event loop below only renders in
            // response to input or a background producer's wake, so
            // without this the first frames the user sees would be
            // mis-sized until they moved the mouse. Pump three forced
            // renders through before entering the loop.
            for(int i = 0; i < 3; ++i)
            {
                render_iteration(ui_result, last_render_ms, /*force=*/true);
            }

            while(true)
            {
                if(event_mgr.dispatch_events())
                {
                    sdl::log_info("shutting down");
                    break;
                }
                render_iteration(ui_result, last_render_ms, /*force=*/false);
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
