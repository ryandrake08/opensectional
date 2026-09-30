#pragma once

namespace osect
{
    class map_widget;
    class user_database;

    // Applies user-waypoint edits requested through the map — create,
    // rename, delete, and drag-to-move. Each edit is surfaced by
    // map_widget as a drained request; waypoint_session writes it
    // through to user.db and notifies map_widget so the next feature
    // build reflects it.
    class waypoint_session
    {
        map_widget& map_;
        user_database& udb_;

    public:
        waypoint_session(map_widget& map, user_database& udb);

        waypoint_session(const waypoint_session&) = delete;
        waypoint_session& operator=(const waypoint_session&) = delete;

        // Drain and apply every pending waypoint edit. Returns true if
        // anything changed and the frame needs to be re-rendered.
        bool process();
    };
}
