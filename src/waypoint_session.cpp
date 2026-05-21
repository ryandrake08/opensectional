#include "waypoint_session.hpp"

#include "map_widget.hpp"
#include "user_database.hpp"
#include <exception>
#include <memory>
#include <optional>
#include <sdl/log.hpp>
#include <string>
#include <utility>

namespace osect
{
    struct waypoint_session::impl
    {
        map_widget& map;
        user_database& udb;

        impl(map_widget& map, user_database& udb) : map(map), udb(udb)
        {
        }

        bool handle_create()
        {
            auto req = map.drain_create_waypoint_request();
            if(!req)
            {
                return false;
            }
            auto [lon, lat] = *req;
            try
            {
                const auto wp = udb.insert_waypoint(lat, lon);
                sdl::log_info("user waypoint created: " + wp.name);
                map.show_waypoint_info(wp);
            }
            catch(const std::exception& e)
            {
                sdl::log_warn(std::string("user.db: insert_waypoint failed: ") + e.what());
            }
            return true;
        }

        bool handle_rename()
        {
            auto req = map.drain_rename_waypoint_request();
            if(!req)
            {
                return false;
            }
            const auto cur = udb.query_waypoint(req->first);
            if(!cur)
            {
                return true; // the waypoint no longer exists
            }
            bool renamed = false;
            try
            {
                // update_waypoint replaces the whole row; keep the
                // current position and change only the name.
                renamed = udb.update_waypoint(req->first, req->second, cur->lat, cur->lon);
            }
            catch(const std::exception& e)
            {
                sdl::log_warn(std::string("user.db: update_waypoint failed: ") + e.what());
                return true;
            }
            // Show what the database now holds — a name collision
            // leaves the old name in place.
            const auto shown = renamed ? user_waypoint{req->first, req->second, cur->lat, cur->lon} : *cur;
            map.show_waypoint_info(shown);
            return true;
        }

        bool handle_delete()
        {
            auto id = map.drain_delete_waypoint_request();
            if(!id)
            {
                return false;
            }
            try
            {
                udb.delete_waypoint(*id);
                sdl::log_info("user waypoint deleted: id=" + std::to_string(*id));
            }
            catch(const std::exception& e)
            {
                sdl::log_warn(std::string("user.db: delete_waypoint failed (continuing): ") + e.what());
            }
            map.notify_waypoints_changed();
            return true;
        }

        bool handle_move()
        {
            auto wp = map.drain_waypoint_drag_result();
            if(!wp)
            {
                return false;
            }
            try
            {
                udb.update_waypoint(wp->waypoint_id, wp->name, wp->lat, wp->lon);
                sdl::log_info("user waypoint repositioned: " + wp->name);
            }
            catch(const std::exception& e)
            {
                sdl::log_warn(std::string("user.db: update_waypoint (drag) failed: ") + e.what());
            }
            map.notify_waypoints_changed();
            return true;
        }
    };

    waypoint_session::waypoint_session(map_widget& map, user_database& udb)
        : pimpl(std::make_unique<impl>(map, udb))
    {
    }

    waypoint_session::~waypoint_session() = default;

    bool waypoint_session::process()
    {
        bool changed = false;
        changed |= pimpl->handle_create();
        changed |= pimpl->handle_rename();
        changed |= pimpl->handle_delete();
        changed |= pimpl->handle_move();
        return changed;
    }
}
