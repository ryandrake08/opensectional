#include "terrain_profile.hpp"

#include "elevation_source.hpp"
#include "flight_route.hpp"
#include "geo_math.hpp"
#include "nasr_database.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

namespace
{
    std::vector<osect::obstacle> obstacles_in_bbox(const osect::nasr_database& airports, const osect::geo_bbox& bbox)
    {
        if(bbox.lon_min >= -180.0 && bbox.lon_max <= 180.0)
        {
            return airports.query_obstacles(bbox);
        }

        std::vector<osect::obstacle> obstacles;
        if(bbox.lon_min < -180.0)
        {
            auto west = airports.query_obstacles({bbox.lon_min + 360.0, bbox.lat_min, 180.0, bbox.lat_max});
            auto east = airports.query_obstacles({-180.0, bbox.lat_min, bbox.lon_max, bbox.lat_max});
            obstacles.insert(obstacles.end(), west.begin(), west.end());
            obstacles.insert(obstacles.end(), east.begin(), east.end());
        }
        else
        {
            auto west = airports.query_obstacles({bbox.lon_min, bbox.lat_min, 180.0, bbox.lat_max});
            auto east = airports.query_obstacles({-180.0, bbox.lat_min, bbox.lon_max - 360.0, bbox.lat_max});
            obstacles.insert(obstacles.end(), west.begin(), west.end());
            obstacles.insert(obstacles.end(), east.begin(), east.end());
        }
        return obstacles;
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

    std::optional<double> airport_elevation_ft(const osect::route_waypoint& waypoint,
                                               const osect::nasr_database& airports)
    {
        if(waypoint.kind != osect::waypoint_kind::airport)
        {
            return std::nullopt;
        }

        const auto matches = airports.lookup_airports(waypoint.id);
        if(matches.empty())
        {
            throw std::runtime_error("cannot resolve airport elevation for " + waypoint.id);
        }
        return matches.front().elev;
    }

    std::optional<double> elevation_at(const osect::route_waypoint* waypoint, double lat, double lon,
                                       const osect::elevation_source& terrain, const osect::nasr_database& airports)
    {
        if(waypoint)
        {
            if(const auto elevation = airport_elevation_ft(*waypoint, airports))
            {
                return elevation;
            }
        }
        return terrain.elevation_ft(lat, lon, terrain.max_zoom());
    }

    struct aircraft_trace
    {
        enum class shape
        {
            trapezoid,
            triangle,
        };

        double route_distance_nm;
        double departure_elevation_ft;
        double arrival_elevation_ft;
        double cruise_altitude_ft;
        osect::terrain_profile_gradients gradients;
        double climb_distance_nm;
        double descent_distance_nm;
        shape profile_shape;

        double altitude_at(double distance_nm) const
        {
            if(profile_shape == shape::trapezoid)
            {
                const double descent_start_nm = route_distance_nm - descent_distance_nm;
                if(distance_nm < climb_distance_nm)
                {
                    return departure_elevation_ft + distance_nm * gradients.climb_ft_per_nm;
                }
                if(distance_nm > descent_start_nm)
                {
                    return arrival_elevation_ft + (route_distance_nm - distance_nm) * gradients.descent_ft_per_nm;
                }
                return cruise_altitude_ft;
            }
            if(distance_nm <= climb_distance_nm)
            {
                return std::min(cruise_altitude_ft, departure_elevation_ft + distance_nm * gradients.climb_ft_per_nm);
            }
            return std::min(cruise_altitude_ft,
                            arrival_elevation_ft + (route_distance_nm - distance_nm) * gradients.descent_ft_per_nm);
        }

        osect::terrain_profile_phase phase_at(double distance_nm) const
        {
            if(profile_shape == shape::trapezoid)
            {
                if(distance_nm < climb_distance_nm)
                {
                    return osect::terrain_profile_phase::climb;
                }
                if(distance_nm > route_distance_nm - descent_distance_nm)
                {
                    return osect::terrain_profile_phase::descent;
                }
                return osect::terrain_profile_phase::cruise;
            }
            return distance_nm <= climb_distance_nm ? osect::terrain_profile_phase::climb
                                                          : osect::terrain_profile_phase::descent;
        }
    };

    std::optional<aircraft_trace> make_aircraft_trace(double route_distance_nm,
                                                       const std::optional<double>& departure_elevation_ft,
                                                       const std::optional<double>& arrival_elevation_ft,
                                                       const std::optional<double>& cruise_altitude_ft,
                                                       const osect::terrain_profile_gradients& gradients)
    {
        if(!cruise_altitude_ft || !departure_elevation_ft || !arrival_elevation_ft)
        {
            return std::nullopt;
        }

        const double climb_distance = (*cruise_altitude_ft - *departure_elevation_ft) / gradients.climb_ft_per_nm;
        const double descent_distance = (*cruise_altitude_ft - *arrival_elevation_ft) / gradients.descent_ft_per_nm;
        if(climb_distance + descent_distance <= route_distance_nm)
        {
            return aircraft_trace{route_distance_nm, *departure_elevation_ft, *arrival_elevation_ft, *cruise_altitude_ft,
                                  gradients, climb_distance, descent_distance, aircraft_trace::shape::trapezoid};
        }

        const double intersection = (*arrival_elevation_ft + gradients.descent_ft_per_nm * route_distance_nm -
                                     *departure_elevation_ft) /
                                    (gradients.climb_ft_per_nm + gradients.descent_ft_per_nm);
        if(intersection < 0.0 || intersection > route_distance_nm)
        {
            throw std::runtime_error("route is too short for the configured climb and descent gradients");
        }
        return aircraft_trace{route_distance_nm, *departure_elevation_ft, *arrival_elevation_ft, *cruise_altitude_ft,
                              gradients, intersection, intersection, aircraft_trace::shape::triangle};
    }
}

namespace osect
{
    terrain_profile build_terrain_profile(const std::vector<route_waypoint>& waypoints, const elevation_source& terrain,
                                          const nasr_database& airports, std::optional<double> cruise_altitude_ft,
                                          terrain_profile_gradients gradients, double corridor_half_width_nm,
                                          bool include_obstacles, double required_clearance_ft)
    {
        if(waypoints.size() < 2)
        {
            throw std::runtime_error("terrain profile requires at least two waypoints");
        }
        if(!std::isfinite(gradients.climb_ft_per_nm) || gradients.climb_ft_per_nm <= 0.0 ||
           !std::isfinite(gradients.descent_ft_per_nm) || gradients.descent_ft_per_nm <= 0.0)
        {
            throw std::runtime_error("terrain profile gradients must be positive");
        }
        if(cruise_altitude_ft && !std::isfinite(*cruise_altitude_ft))
        {
            throw std::runtime_error("terrain profile cruise altitude must be finite");
        }
        if(!std::isfinite(corridor_half_width_nm) || corridor_half_width_nm <= 0.0)
        {
            throw std::runtime_error("terrain profile corridor half-width must be positive");
        }
        if(!std::isfinite(required_clearance_ft) || required_clearance_ft < 0.0)
        {
            throw std::runtime_error("terrain profile required clearance must be non-negative");
        }

        terrain_profile profile;
        std::vector<airspace_point> sample_points;
        std::vector<std::pair<std::size_t, std::size_t>> leg_sample_ranges;
        double total_route_distance_nm = 0.0;
        for(std::size_t leg_index = 0; leg_index + 1 < waypoints.size(); ++leg_index)
        {
            const auto& from = waypoints[leg_index];
            const auto& to = waypoints[leg_index + 1];
            total_route_distance_nm += haversine_distance_nm(from.lat, from.lon, to.lat, to.lon);
        }
        const double sample_interval_nm = total_route_distance_nm > 0.0
                                              ? std::min(TERRAIN_PROFILE_SAMPLE_INTERVAL_NM,
                                                         total_route_distance_nm / (TERRAIN_PROFILE_MIN_SAMPLES - 1))
                                              : TERRAIN_PROFILE_SAMPLE_INTERVAL_NM;

        double route_distance_nm = 0.0;
        for(std::size_t leg_index = 0; leg_index + 1 < waypoints.size(); ++leg_index)
        {
            const auto& from = waypoints[leg_index];
            const auto& to = waypoints[leg_index + 1];
            const double leg_distance_nm = haversine_distance_nm(from.lat, from.lon, to.lat, to.lon);
            const double leg_start_nm = route_distance_nm;
            const auto points = geodesic_interpolate(from.lat, from.lon, to.lat, to.lon, sample_interval_nm);
            const std::size_t first_sample = profile.samples.size();

            for(std::size_t point_index = 0; point_index < points.size(); ++point_index)
            {
                if(leg_index > 0 && point_index == 0)
                {
                    continue;
                }
                const route_waypoint* waypoint = nullptr;
                if(point_index == 0)
                {
                    waypoint = &from;
                }
                else if(point_index + 1 == points.size())
                {
                    waypoint = &to;
                }

                double distance_nm = leg_start_nm;
                if(point_index + 1 == points.size())
                {
                    distance_nm += leg_distance_nm;
                }
                else if(point_index > 0)
                {
                    distance_nm += haversine_distance_nm(from.lat, from.lon, points[point_index].lat,
                                                         points[point_index].lon);
                }
                profile.samples.push_back({distance_nm,
                                           elevation_at(waypoint, points[point_index].lat, points[point_index].lon,
                                           terrain, airports), std::nullopt, std::nullopt, false, false});
                sample_points.push_back(points[point_index]);
            }

            route_distance_nm += leg_distance_nm;
            profile.legs.push_back({leg_index, leg_start_nm, route_distance_nm, std::nullopt, std::nullopt, false});
            leg_sample_ranges.emplace_back(first_sample, profile.samples.size());
        }

        std::vector<geo_bbox> windows;
        windows.reserve(profile.samples.size());
        for(std::size_t sample_index = 0; sample_index < profile.samples.size(); ++sample_index)
        {
            double half_step_nm = 0.0;
            if(sample_index > 0)
            {
                half_step_nm = (profile.samples[sample_index].distance_nm -
                                profile.samples[sample_index - 1].distance_nm) *
                               0.5;
            }
            if(sample_index + 1 < profile.samples.size())
            {
                half_step_nm = std::max(half_step_nm, (profile.samples[sample_index + 1].distance_nm -
                                                        profile.samples[sample_index].distance_nm) *
                                                           0.5);
            }
            const geo_bbox window = bbox_around(sample_points[sample_index].lat, sample_points[sample_index].lon,
                                                corridor_half_width_nm + half_step_nm);
            windows.push_back(window);
            profile.samples[sample_index].corridor_elevation_ft =
                terrain.maximum_elevation_ft(window.lat_min, window.lon_min, window.lat_max, window.lon_max);
        }

        if(include_obstacles)
        {
            for(const auto& [first_sample, end_sample] : leg_sample_ranges)
            {
                geo_bbox leg_bbox = windows[first_sample];
                for(std::size_t sample_index = first_sample + 1; sample_index < end_sample; ++sample_index)
                {
                    leg_bbox.lon_min = std::min(leg_bbox.lon_min, windows[sample_index].lon_min);
                    leg_bbox.lat_min = std::min(leg_bbox.lat_min, windows[sample_index].lat_min);
                    leg_bbox.lon_max = std::max(leg_bbox.lon_max, windows[sample_index].lon_max);
                    leg_bbox.lat_max = std::max(leg_bbox.lat_max, windows[sample_index].lat_max);
                }

                const auto obstacles = obstacles_in_bbox(airports, leg_bbox);
                for(std::size_t sample_index = first_sample; sample_index < end_sample; ++sample_index)
                {
                    for(const auto& obstacle : obstacles)
                    {
                        if(!inside_bbox(obstacle, windows[sample_index]))
                        {
                            continue;
                        }
                        profile.samples[sample_index].has_obstacle = true;
                        const auto obstacle_elevation_ft = static_cast<double>(obstacle.amsl_ht);
                        const bool obstacle_only = !profile.samples[sample_index].corridor_elevation_ft ||
                                                   profile.samples[sample_index].corridor_from_obstacle;
                        if(!profile.samples[sample_index].corridor_elevation_ft ||
                           obstacle_elevation_ft > *profile.samples[sample_index].corridor_elevation_ft)
                        {
                            profile.samples[sample_index].corridor_elevation_ft = obstacle_elevation_ft;
                            profile.samples[sample_index].corridor_from_obstacle = obstacle_only;
                        }
                    }
                }
            }
        }

        const auto departure_elevation_ft = profile.samples.front().centreline_elevation_ft;
        const auto arrival_elevation_ft = profile.samples.back().centreline_elevation_ft;
        if(cruise_altitude_ft &&
           ((departure_elevation_ft && *cruise_altitude_ft < *departure_elevation_ft) ||
            (arrival_elevation_ft && *cruise_altitude_ft < *arrival_elevation_ft)))
        {
            throw std::runtime_error("terrain profile cruise altitude is below an endpoint elevation");
        }
        const auto aircraft_trace = make_aircraft_trace(route_distance_nm, departure_elevation_ft, arrival_elevation_ft,
                                                        cruise_altitude_ft, gradients);
        for(auto& sample : profile.samples)
        {
            if(aircraft_trace)
            {
                sample.aircraft_altitude_ft = aircraft_trace->altitude_at(sample.distance_nm);
            }
        }

        for(auto& leg : profile.legs)
        {
            for(const auto& sample : profile.samples)
            {
                if(sample.distance_nm < leg.start_distance_nm || sample.distance_nm > leg.end_distance_nm ||
                   !sample.corridor_elevation_ft)
                {
                    continue;
                }
                if(!leg.maximum_elevation_ft || *sample.corridor_elevation_ft > *leg.maximum_elevation_ft)
                {
                    leg.maximum_elevation_ft = sample.corridor_elevation_ft;
                }
            }
            if(leg.maximum_elevation_ft)
            {
                leg.msa_ft = *leg.maximum_elevation_ft + required_clearance_ft;
            }
        }

        std::optional<terrain_profile_clearance_span> span;
        for(const auto& sample : profile.samples)
        {
            if(sample.corridor_elevation_ft &&
               (!profile.maximum_elevation_ft || *sample.corridor_elevation_ft > *profile.maximum_elevation_ft))
            {
                profile.maximum_elevation_ft = sample.corridor_elevation_ft;
            }

            std::optional<terrain_profile_phase> phase;
            if(aircraft_trace)
            {
                phase = aircraft_trace->phase_at(sample.distance_nm);
            }
            const bool violates = phase && sample.aircraft_altitude_ft && sample.corridor_elevation_ft &&
                                  *sample.aircraft_altitude_ft < *sample.corridor_elevation_ft + required_clearance_ft;
            if(violates && (!span || span->phase == *phase))
            {
                if(!span)
                {
                    span = {sample.distance_nm, sample.distance_nm, *phase};
                }
                else
                {
                    span->end_distance_nm = sample.distance_nm;
                }
                continue;
            }
            if(span)
            {
                profile.clearance_spans.push_back(*span);
                span.reset();
            }
            if(violates)
            {
                span = {sample.distance_nm, sample.distance_nm, *phase};
            }
        }
        if(span)
        {
            profile.clearance_spans.push_back(*span);
        }
        return profile;
    }
} // namespace osect
