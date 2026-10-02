#include "route_session.hpp"
#include "elevation_source.hpp"
#include "flight_route.hpp"
#include "ini_config.hpp"
#include "map_widget.hpp"
#include "route_plan_config.hpp"
#include "route_submitter.hpp"
#include "terrain_profile_worker.hpp"
#include "terrain_style.hpp"
#include "ui_overlay.hpp"
#include "user_database.hpp"
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
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
        std::optional<terrain_profile_worker> profile_worker;
        route_plan_options plan_options;
        // Maps each route-panel tab id to its planned route's
        // persistent route_id. Tabs without a planned route are
        // absent. route_id is stable across mutations (no
        // shift-after-remove housekeeping needed).
        std::unordered_map<std::uint64_t, route_id> tab_to_route;
        struct profile_request
        {
            route_id id;
            std::uint64_t generation;
            std::vector<route_waypoint> waypoints;
            std::optional<double> cruise_altitude_ft;
            terrain_profile_gradients gradients;
        };
        std::deque<profile_request> profile_queue;
        std::optional<profile_request> active_profile;
        std::unordered_map<route_id, std::uint64_t> profile_generations;
        std::unordered_map<route_id, terrain_profile> profiles;
        std::unordered_map<route_id, std::string> profile_errors;
        // Last active panel tab id observed via active_tab_changed.
        // Used to decide whether a freshly-planned route should pull
        // the view (only when the user is still focused on the
        // submitting tab when the result arrives).
        std::uint64_t active_tab_id = 0;

        impl(ui_overlay& ui, map_widget& map, user_database& udb, const elevation_source& terrain,
             const ini_config& ini, const std::filesystem::path& db_path)
            : ui(ui),
              map(map),
              udb(udb),
              submitter(db_path, terrain),
              profile_worker(terrain.available() ? std::make_optional<terrain_profile_worker>(
                                                       terrain, db_path, terrain_style(ini).margins)
                                                 : std::nullopt),
              plan_options(load_route_plan_options(ini))
        {
            // Without terrain data, terrain avoidance is off.
            plan_options.avoid_terrain = plan_options.avoid_terrain && terrain.available();
            plan_options.margins = terrain_style(ini).margins;
            ui.set_route_planner_defaults(plan_options.max_leg_length_nm, plan_options.use_airways,
                                          plan_options.avoid_terrain);
        }

        void remove_queued_profile(route_id id)
        {
            for(auto it = profile_queue.begin(); it != profile_queue.end();)
            {
                if(it->id == id)
                {
                    it = profile_queue.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }

        void queue_profile(route_id id, const flight_route& route, std::optional<double> cruise_altitude_ft,
                           terrain_profile_gradients gradients)
        {
            const auto generation = ++profile_generations[id];
            profiles.erase(id);
            profile_errors.erase(id);
            remove_queued_profile(id);
            if(!profile_worker)
            {
                return;
            }
            profile_queue.push_back({id, generation, route.waypoints, cruise_altitude_ft, gradients});
        }

        void drop_profile(route_id id)
        {
            profile_generations.erase(id);
            profiles.erase(id);
            profile_errors.erase(id);
            remove_queued_profile(id);
        }

        bool profile_is_current(const profile_request& request) const
        {
            const auto it = profile_generations.find(request.id);
            return it != profile_generations.end() && it->second == request.generation;
        }

        bool service_profiles()
        {
            bool changed = false;
            if(active_profile)
            {
                auto status = profile_worker->poll();
                if(status.pending)
                {
                    return false;
                }
                const auto request = std::move(*active_profile);
                active_profile.reset();
                if(status.result)
                {
                    if(profile_is_current(request))
                    {
                        profiles[request.id] = std::move(*status.result);
                        changed = true;
                    }
                }
                else if(!status.error.empty() && profile_is_current(request))
                {
                    sdl::log_warn("terrain profile: route_id=" + std::to_string(request.id) + " " + status.error);
                    profile_errors[request.id] = std::move(status.error);
                }
            }
            if(!active_profile && !profile_queue.empty())
            {
                active_profile = std::move(profile_queue.front());
                profile_queue.pop_front();
                profile_worker->submit(active_profile->waypoints, active_profile->cruise_altitude_ft,
                                       active_profile->gradients);
            }
            return changed;
        }

        void sync_terrain_warning_overlay()
        {
            if(!ui.terrain_profile_open())
            {
                map.set_terrain_warning_overlay(std::nullopt, {});
                return;
            }
            const auto id = map.active_route();
            const auto tab = tab_to_route.find(active_tab_id);
            const auto profile = tab == tab_to_route.end() ? profiles.end() : profiles.find(tab->second);
            if(!id || profile == profiles.end())
            {
                map.set_terrain_warning_overlay(std::nullopt, {});
                return;
            }
            map.set_terrain_warning_overlay(id, profile->second.clearance_spans);
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
                    auto tab_id = ui.add_route_tab(route, rec.cruise_altitude_ft);
                    map.add_route(rec.route_id);
                    tab_to_route.emplace(tab_id, rec.route_id);
                    queue_profile(rec.route_id, route, rec.cruise_altitude_ft, rec.gradients);
                    ++loaded;
                }
                catch(const std::exception& e)
                {
                    sdl::log_warn("user.db: route_id=" + std::to_string(rec.route_id) + " failed to load: " + e.what() +
                                  " — row retained, not loaded");
                    ++skipped;
                }
            }
            sdl::log_info("user.db: restored " + std::to_string(loaded) + " routes (" + std::to_string(skipped) +
                          " skipped)");
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
                    drop_profile(rid);
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
            // plan_options; we just overlay max_leg, use-airways,
            // avoid-terrain, and the cruise altitude from the panel
            // that submitted.
            auto opts = plan_options;
            opts.max_leg_length_nm = req.max_leg_nm;
            opts.use_airways = req.use_airways;
            opts.avoid_terrain = req.avoid_terrain;
            opts.cruise_altitude_ft = ui.cruise_altitude_ft(req.tab_id);
            if(auto err = validate_route_plan_options(opts); !err.empty())
            {
                sdl::log_warn("route submit rejected: " + err);
                ui.clear_route_state(req.tab_id, err);
                return true;
            }
            sdl::log_info("route submit: tab=" + std::to_string(req.tab_id) + " \"" + req.text +
                          "\" (max_leg=" + std::to_string(static_cast<int>(opts.max_leg_length_nm)) +
                          "nm airways=" + (opts.use_airways ? "true" : "false") +
                          " avoid_terrain=" + (opts.avoid_terrain ? "true" : "false") + ")");
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
                if(status.completion->terrain_unchecked_nm > 0.0)
                {
                    sdl::log_warn("route planned: tab=" + std::to_string(tag) + " terrain not checked along " +
                                  std::to_string(static_cast<int>(std::ceil(status.completion->terrain_unchecked_nm))) +
                                  " nm of planned legs outside terrain coverage");
                }
                ui.set_route_state(tag, route);
                ui.set_route_terrain_unchecked(tag, status.completion->terrain_unchecked_nm);

                auto it = tab_to_route.find(tag);
                route_id rid = 0;
                terrain_profile_gradients gradients;
                bool route_saved = false;
                if(it != tab_to_route.end())
                {
                    rid = it->second;
                    try
                    {
                        if(const auto rec = udb.query_route(rid))
                        {
                            gradients = rec->gradients;
                        }
                        udb.update_route(rid, route.to_rows(), ui.cruise_altitude_ft(tag));
                        route_saved = true;
                    }
                    catch(const std::exception& e)
                    {
                        sdl::log_warn(std::string("user.db: update_route failed (continuing): ") + e.what());
                    }
                    if(route_saved)
                    {
                        map.replace_route(rid);
                    }
                }
                else
                {
                    try
                    {
                        rid = udb.insert_route(route.to_rows(), ui.cruise_altitude_ft(tag), gradients);
                        route_saved = true;
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
                if(route_saved)
                {
                    queue_profile(rid, route, ui.cruise_altitude_ft(tag), gradients);
                }

                // Only pull the view, activate, and highlight if the
                // submitting tab is still the user's focused tab —
                // otherwise the map stays where the user has it.
                if(tag == active_tab_id)
                {
                    map.set_active_route(rid);
                    const auto altitude = ui.cruise_altitude_ft(tag);
                    map.set_cruise_altitude_ft(altitude ? std::optional<float>(static_cast<float>(*altitude))
                                                        : std::nullopt);
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
            auto tab = tab_for_route(*active);
            if(!tab)
            {
                return true;
            }
            try
            {
                terrain_profile_gradients gradients;
                if(const auto rec = udb.query_route(*active))
                {
                    gradients = rec->gradients;
                }
                udb.update_route(*active, result->to_rows(), ui.cruise_altitude_ft(*tab));
                queue_profile(*active, *result, ui.cruise_altitude_ft(*tab), gradients);
            }
            catch(const std::exception& e)
            {
                // On write failure, user.db keeps the prior text;
                // the next feature build will render the old route.
                // Leave the tab's text alone too so disk and UI agree.
                sdl::log_warn(std::string("user.db: drag update_route failed: ") + e.what());
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
            const auto altitude = ui.cruise_altitude_ft(active_tab_id);
            map.set_cruise_altitude_ft(altitude ? std::optional<float>(static_cast<float>(*altitude)) : std::nullopt);
            // Tab switch is the user's intentional focus gesture, so
            // align selection with the new tab's route. They can
            // diverge again by clicking a different route on the map.
            map.select_route(rid);
            return true;
        }

        bool handle_cruise_altitude_changed(const ui_overlay_result& r)
        {
            if(!r.cruise_altitude_changed || r.cruise_altitude_changed->first != active_tab_id)
            {
                return false;
            }
            const auto altitude = r.cruise_altitude_changed->second;
            map.set_cruise_altitude_ft(altitude ? std::optional<float>(static_cast<float>(*altitude)) : std::nullopt);
            const auto it = tab_to_route.find(active_tab_id);
            if(it == tab_to_route.end())
            {
                return true;
            }
            try
            {
                udb.update_route_cruise_altitude(it->second, altitude);
            }
            catch(const std::exception& e)
            {
                sdl::log_warn(std::string("user.db: update_route_cruise_altitude failed (continuing): ") + e.what());
                return true;
            }

            try
            {
                const auto rec = udb.query_route(it->second);
                if(rec)
                {
                    queue_profile(it->second, flight_route(rec->waypoints), altitude, rec->gradients);
                }
            }
            catch(const std::exception& e)
            {
                sdl::log_warn(std::string("terrain profile: route refresh after cruise altitude change failed: ") +
                              e.what());
            }
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
                drop_profile(rid);
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
            drop_profile(*rid);
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

        bool handle_route_profile_request()
        {
            if(!map.drain_route_profile_request())
            {
                return false;
            }
            ui.toggle_terrain_profile();
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
            const auto altitude = ui.cruise_altitude_ft(*tab);
            map.set_cruise_altitude_ft(altitude ? std::optional<float>(static_cast<float>(*altitude)) : std::nullopt);
            map.select_route(*rid);
            return true;
        }
    };

    route_session::route_session(ui_overlay& ui, map_widget& map, user_database& udb, const elevation_source& terrain,
                                 const ini_config& ini, const std::filesystem::path& db_path)
        : pimpl(std::make_unique<impl>(ui, map, udb, terrain, ini, db_path))
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
        changed |= pimpl->handle_cruise_altitude_changed(r);
        changed |= pimpl->handle_route_request(r);
        changed |= pimpl->handle_route_status();
        changed |= pimpl->handle_route_dirty();
        changed |= pimpl->handle_route_delete_request();
        changed |= pimpl->handle_route_profile_request();
        changed |= pimpl->handle_route_activate_request();
        changed |= pimpl->service_profiles();
        pimpl->sync_terrain_warning_overlay();
        return changed;
    }

    const terrain_profile* route_session::profile_for_route(std::int64_t route_id) const
    {
        const auto it = pimpl->profiles.find(route_id);
        return it != pimpl->profiles.end() ? &it->second : nullptr;
    }

    const terrain_profile* route_session::active_profile() const
    {
        const auto route = pimpl->tab_to_route.find(pimpl->active_tab_id);
        return route == pimpl->tab_to_route.end() ? nullptr : profile_for_route(route->second);
    }

    std::optional<std::string> route_session::active_profile_error() const
    {
        const auto route = pimpl->tab_to_route.find(pimpl->active_tab_id);
        if(route == pimpl->tab_to_route.end())
        {
            return std::nullopt;
        }
        const auto error = pimpl->profile_errors.find(route->second);
        return error == pimpl->profile_errors.end() ? std::nullopt : std::optional<std::string>{error->second};
    }

    bool route_session::has_active_route() const
    {
        return pimpl->tab_to_route.find(pimpl->active_tab_id) != pimpl->tab_to_route.end();
    }
}
