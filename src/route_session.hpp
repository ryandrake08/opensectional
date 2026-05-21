#pragma once

#include <memory>

class ini_config;

namespace osect
{
    class map_widget;
    class ui_overlay;
    class user_database;
    struct ui_overlay_result;

    // Owns the correspondence between route-panel tabs, the routes
    // rendered on the map, and the route rows in user.db. Translates
    // panel submissions, tab changes, planner completions, drag
    // commits, and map-click activate/delete into the matching
    // mutations across ui_overlay, map_widget, user.db, and the
    // background route planner.
    class route_session
    {
        struct impl;
        std::unique_ptr<impl> pimpl;

    public:
        route_session(ui_overlay& ui, map_widget& map, user_database& udb, const ini_config& ini,
                       const char* db_path);
        ~route_session();

        route_session(const route_session&) = delete;
        route_session& operator=(const route_session&) = delete;

        // Load the routes persisted in user.db, seeding map_widget and
        // a route-panel tab for each. Called once at startup.
        void restore_from_db();

        // Drain and apply this frame's route events. `r` carries the
        // panel-originated events; map-originated ones (drag commits,
        // popup activate/delete, planner completions) are drained from
        // map_widget and the planner. Returns true if anything changed
        // and the frame needs to be re-rendered.
        bool process(const ui_overlay_result& r);
    };
}
