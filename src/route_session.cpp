#include "route_session.hpp"

#include "flight_route.hpp"
#include "ini_config.hpp"
#include "map_widget.hpp"
#include "route_plan_config.hpp"
#include "route_submitter.hpp"
#include "ui_overlay.hpp"
#include "user_database.hpp"
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <sdl/log.hpp>
#include <string>
#include <unordered_map>
#include <utility>

namespace osect
{
    struct route_session::impl
    {
        ui_overlay& ui;
        map_widget& map;
        user_database& udb;
        route_submitter submitter;
        route_plan_options plan_options;
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

        impl(ui_overlay& ui, map_widget& map, user_database& udb, const ini_config& ini, const char* db_path)
            : ui(ui), map(map), udb(udb), submitter(db_path), plan_options(load_route_plan_options(ini))
        {
            ui.set_route_planner_defaults(plan_options.max_leg_length_nm, plan_options.use_airways);
        }

        void restore_from_db()
        {
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
    };

    route_session::route_session(ui_overlay& ui, map_widget& map, user_database& udb, const ini_config& ini,
                                 const char* db_path)
        : pimpl(std::make_unique<impl>(ui, map, udb, ini, db_path))
    {
    }

    route_session::~route_session() = default;

    void route_session::restore_from_db()
    {
        pimpl->restore_from_db();
    }

    bool route_session::process(const ui_overlay_result& r)
    {
        bool changed = false;
        changed |= pimpl->handle_tab_closed(r);
        changed |= pimpl->handle_active_tab_changed(r);
        changed |= pimpl->handle_route_request(r);
        changed |= pimpl->handle_route_status();
        changed |= pimpl->handle_route_dirty();
        changed |= pimpl->handle_route_delete_request();
        changed |= pimpl->handle_route_activate_request();
        return changed;
    }
}
