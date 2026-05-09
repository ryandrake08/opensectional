#pragma once

#include "flight_route.hpp"
#include <cassert>
#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

namespace osect
{
    enum class route_drag_mode
    {
        none,
        segment,
        waypoint
    };

    // Route-related state that map_widget owns: the route list, the
    // two role indexes (active = panel/drag target, selected =
    // highlighted + popup), in-progress drag state, and the
    // pending-event flags drained by the caller. Pure data plus
    // helpers that depend only on the routes vector — anything
    // needing the map view, pick db, popups, or feature renderer
    // stays on map_widget.
    struct route_store
    {
        std::vector<flight_route> routes;
        // Index of the active route — the panel/drag target, driven
        // by which tab is focused. The "selected" route (highlighted
        // with white + halos and shown in the route info popup) is
        // tracked through feature_renderer::selection() as a
        // route_pick variant; map_widget queries that when it needs
        // to know which route the popup belongs to.
        std::optional<std::size_t> active_route_index;
        // True once a route has been mutated internally (e.g. drag
        // insert / replace / delete) since the last drain. Caller
        // re-pushes panel state.
        bool dirty = false;
        // Route index the user requested be deleted via the popup's
        // Delete button. Drained by the caller, which owns the
        // route's lifecycle (so the corresponding panel tab can be
        // closed in the same operation).
        std::optional<std::size_t> delete_request;
        // Non-active route the user clicked, since the last drain.
        // Caller switches the panel tab to the matching tab.
        std::optional<std::size_t> activate_request;
        // In-progress drag state. mode == none when no drag is
        // active. index is the segment index in segment mode or the
        // waypoint index in waypoint mode.
        struct
        {
            route_drag_mode mode = route_drag_mode::none;
            std::size_t index = 0;
        } drag;

        flight_route* active()
        {
            if(!active_route_index || *active_route_index >= routes.size())
            {
                return nullptr;
            }
            return &routes[*active_route_index];
        }
        const flight_route* active() const
        {
            if(!active_route_index || *active_route_index >= routes.size())
            {
                return nullptr;
            }
            return &routes[*active_route_index];
        }

        // Geographic centroid of the route at `index`. Asserts on
        // out-of-range.
        std::pair<double, double> centroid(std::size_t index) const
        {
            assert(index < routes.size());
            double sum_lon = 0.0;
            double sum_lat = 0.0;
            const auto& wps = routes[index].waypoints;
            for(const auto& wp : wps)
            {
                sum_lon += waypoint_lon(wp);
                sum_lat += waypoint_lat(wp);
            }
            auto n = static_cast<double>(wps.size());
            return {sum_lon / n, sum_lat / n};
        }
    };
} // namespace osect
