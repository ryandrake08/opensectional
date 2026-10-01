#include "terrain_corridor.hpp"
#include "elevation_source.hpp"
#include "geo_math.hpp"
#include "nasr_database.hpp"
#include "terrain_profile.hpp"
#include <algorithm>

namespace
{
    std::vector<osect::obstacle> obstacles_in_bbox(const osect::nasr_database& obstacles, const osect::geo_bbox& bbox)
    {
        if(bbox.lon_min >= -180.0 && bbox.lon_max <= 180.0)
        {
            return obstacles.query_obstacles(bbox);
        }

        std::vector<osect::obstacle> result;
        if(bbox.lon_min < -180.0)
        {
            auto west = obstacles.query_obstacles({bbox.lon_min + 360.0, bbox.lat_min, 180.0, bbox.lat_max});
            auto east = obstacles.query_obstacles({-180.0, bbox.lat_min, bbox.lon_max, bbox.lat_max});
            result.insert(result.end(), west.begin(), west.end());
            result.insert(result.end(), east.begin(), east.end());
        }
        else
        {
            auto west = obstacles.query_obstacles({bbox.lon_min, bbox.lat_min, 180.0, bbox.lat_max});
            auto east = obstacles.query_obstacles({-180.0, bbox.lat_min, bbox.lon_max - 360.0, bbox.lat_max});
            result.insert(result.end(), west.begin(), west.end());
            result.insert(result.end(), east.begin(), east.end());
        }
        return result;
    }

    bool inside_bbox(const osect::obstacle& obstacle, const osect::geo_bbox& bbox)
    {
        double longitude = obstacle.lon;
        while(longitude < bbox.lon_min)
        {
            longitude += 360.0;
        }
        while(longitude > bbox.lon_max)
        {
            longitude -= 360.0;
        }
        return obstacle.lat >= bbox.lat_min && obstacle.lat <= bbox.lat_max && longitude >= bbox.lon_min &&
               longitude <= bbox.lon_max;
    }

    osect::geo_bbox bbox_union(const osect::geo_bbox& a, const osect::geo_bbox& b)
    {
        return {std::min(a.lon_min, b.lon_min), std::min(a.lat_min, b.lat_min), std::max(a.lon_max, b.lon_max),
                std::max(a.lat_max, b.lat_max)};
    }
}

namespace osect
{
    double corridor_half_width_nm(double corridor_width_nm, double distance_from_start_nm, double distance_to_end_nm)
    {
        const auto distance_from_terminal_nm = std::min(distance_from_start_nm, distance_to_end_nm);
        return corridor_width_nm / 2.0 *
               std::clamp(distance_from_terminal_nm / TERRAIN_PROFILE_TERMINAL_CORRIDOR_DISTANCE_NM, 0.0, 1.0);
    }

    std::vector<corridor_interval> corridor_intervals(const std::vector<corridor_station>& stations,
                                                      const elevation_source& terrain, const nasr_database& obstacles,
                                                      bool include_obstacles)
    {
        std::vector<geo_bbox> windows;
        std::vector<corridor_interval> intervals;
        for(std::size_t station_index = 0; station_index + 1 < stations.size(); ++station_index)
        {
            const auto& start = stations[station_index];
            const auto& end = stations[station_index + 1];
            const geo_bbox window = bbox_union(bbox_around(start.point.lat, start.point.lon, start.half_width_nm),
                                               bbox_around(end.point.lat, end.point.lon, end.half_width_nm));
            windows.push_back(window);
            intervals.push_back(
                {window, terrain.maximum_elevation_ft(window.lat_min, window.lon_min, window.lat_max, window.lon_max),
                 std::nullopt, terrain.covers(window.lat_min, window.lon_min, window.lat_max, window.lon_max)});
        }

        // Obstacles are fetched for runs of consecutive intervals and
        // assigned to each interval whose window contains them.
        for(std::size_t first = 0; include_obstacles && first < windows.size();)
        {
            geo_bbox run_window = windows[first];
            std::size_t end = first + 1;
            while(end < windows.size() &&
                  stations[end + 1].distance_nm - stations[first].distance_nm <= TERRAIN_PROFILE_OBSTACLE_QUERY_SPAN_NM)
            {
                run_window = bbox_union(run_window, windows[end]);
                ++end;
            }
            for(const auto& obstacle : obstacles_in_bbox(obstacles, run_window))
            {
                const auto height_ft = static_cast<double>(obstacle.amsl_ht);
                for(std::size_t interval = first; interval < end; ++interval)
                {
                    auto& maximum_ft = intervals[interval].obstacle_ft;
                    if(inside_bbox(obstacle, windows[interval]) && (!maximum_ft || height_ft > *maximum_ft))
                    {
                        maximum_ft = height_ft;
                    }
                }
            }
            first = end;
        }
        return intervals;
    }
} // namespace osect
