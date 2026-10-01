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

    // Along-track span of the consecutive station intervals whose
    // obstacles are fetched in one database query.
    constexpr double OBSTACLE_QUERY_SPAN_NM = 20.0;

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

    // `lon` shifted by a whole turn to within 180 degrees of `reference`.
    double unwrap_longitude(double lon, double reference)
    {
        if(lon - reference > 180.0)
        {
            return lon - 360.0;
        }
        if(lon - reference < -180.0)
        {
            return lon + 360.0;
        }
        return lon;
    }

    osect::geo_bbox bbox_union(const osect::geo_bbox& a, const osect::geo_bbox& b)
    {
        return {std::min(a.lon_min, b.lon_min), std::min(a.lat_min, b.lat_min), std::max(a.lon_max, b.lon_max),
                std::max(a.lat_max, b.lat_max)};
    }

    std::optional<double> optional_max(const std::optional<double>& a, const std::optional<double>& b)
    {
        if(!a || !b)
        {
            return a ? a : b;
        }
        return std::max(*a, *b);
    }

    struct profile_station
    {
        double distance_nm;
        osect::airspace_point point;
        std::optional<double> elevation_ft;
    };

    // Places centreline stations along one great-circle leg. Station
    // longitudes are continuous with `from`.
    struct leg_stations
    {
        osect::airspace_point from;
        osect::airspace_point to;
        double start_nm;
        double distance_nm;
        double min_interval_nm;
        const osect::elevation_source& terrain;

        profile_station at(double distance) const
        {
            const auto point =
                osect::geodesic_point(from.lat, from.lon, to.lat, to.lon, (distance - start_nm) / distance_nm);
            return {distance, point, terrain.elevation_ft(point.lat, point.lon, terrain.max_zoom())};
        }

        // Appends stations after `start` up to and including `end`. An
        // interval is halved while the conservative terrain maximum over
        // it exceeds its lower endpoint by more than the tolerance, or
        // while it has terrain data that an endpoint lacks. The box is
        // padded by one finest-level texel, which covers the texels that
        // bilinear point sampling reads.
        void refine(const profile_station& start, const profile_station& end,
                    std::vector<profile_station>& stations) const
        {
            if(end.distance_nm - start.distance_nm > min_interval_nm)
            {
                const double texel_deg =
                    terrain.available() ? 360.0 / (static_cast<double>(1 << terrain.max_zoom()) * terrain.tile_size())
                                        : 0.0;
                const auto maximum_ft =
                    terrain.maximum_elevation_ft(std::min(start.point.lat, end.point.lat) - texel_deg,
                                                 std::min(start.point.lon, end.point.lon) - texel_deg,
                                                 std::max(start.point.lat, end.point.lat) + texel_deg,
                                                 std::max(start.point.lon, end.point.lon) + texel_deg);
                const bool within_tolerance =
                    !maximum_ft || (start.elevation_ft && end.elevation_ft &&
                                    *maximum_ft - std::min(*start.elevation_ft, *end.elevation_ft) <=
                                        osect::TERRAIN_PROFILE_ELEVATION_TOLERANCE_FT);
                if(!within_tolerance)
                {
                    const auto middle = at((start.distance_nm + end.distance_nm) * 0.5);
                    refine(start, middle, stations);
                    refine(middle, end, stations);
                    return;
                }
            }
            stations.push_back(end);
        }
    };

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
            return aircraft_trace{
                route_distance_nm, *departure_elevation_ft, *arrival_elevation_ft, *cruise_altitude_ft,
                gradients,         climb_distance,          descent_distance,      aircraft_trace::shape::trapezoid};
        }

        const double intersection =
            (*arrival_elevation_ft + gradients.descent_ft_per_nm * route_distance_nm - *departure_elevation_ft) /
            (gradients.climb_ft_per_nm + gradients.descent_ft_per_nm);
        if(intersection < 0.0 || intersection > route_distance_nm)
        {
            throw std::runtime_error("route is too short for the configured climb and descent gradients");
        }
        return aircraft_trace{route_distance_nm,
                              *departure_elevation_ft,
                              *arrival_elevation_ft,
                              *cruise_altitude_ft,
                              gradients,
                              intersection,
                              intersection,
                              aircraft_trace::shape::triangle};
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

        double route_distance_nm = 0.0;
        for(std::size_t leg_index = 0; leg_index + 1 < waypoints.size(); ++leg_index)
        {
            const auto& from = waypoints[leg_index];
            const auto& to = waypoints[leg_index + 1];
            route_distance_nm += haversine_distance_nm(from.lat, from.lon, to.lat, to.lon);
        }
        const double min_samples_interval_nm = route_distance_nm > 0.0
                                                   ? route_distance_nm / (TERRAIN_PROFILE_MIN_SAMPLES - 1)
                                                   : TERRAIN_PROFILE_MAX_SAMPLE_INTERVAL_NM;
        const double min_interval_nm = std::min(TERRAIN_PROFILE_SAMPLE_INTERVAL_NM, min_samples_interval_nm);
        // A power-of-two multiple of the minimum, so halving lands on it.
        const double max_interval_nm =
            min_interval_nm *
            std::exp2(std::floor(std::log2(std::min(TERRAIN_PROFILE_MAX_SAMPLE_INTERVAL_NM, min_samples_interval_nm) /
                                           min_interval_nm)));

        const auto& departure = waypoints.front();
        const auto& arrival = waypoints.back();
        const auto departure_elevation_ft = elevation_at(&departure, departure.lat, departure.lon, terrain, airports);
        const auto arrival_elevation_ft = elevation_at(&arrival, arrival.lat, arrival.lon, terrain, airports);
        if(cruise_altitude_ft && ((departure_elevation_ft && *cruise_altitude_ft < *departure_elevation_ft) ||
                                  (arrival_elevation_ft && *cruise_altitude_ft < *arrival_elevation_ft)))
        {
            throw std::runtime_error("terrain profile cruise altitude is below an endpoint elevation");
        }
        const auto aircraft_trace = make_aircraft_trace(route_distance_nm, departure_elevation_ft, arrival_elevation_ft,
                                                        cruise_altitude_ft, gradients);

        // Stations are always placed where the corridor taper ends and
        // where the aircraft trace changes slope.
        std::vector<double> required_distances_nm{TERRAIN_PROFILE_TERMINAL_CORRIDOR_DISTANCE_NM,
                                                  route_distance_nm - TERRAIN_PROFILE_TERMINAL_CORRIDOR_DISTANCE_NM};
        if(aircraft_trace)
        {
            required_distances_nm.push_back(aircraft_trace->climb_distance_nm);
            if(aircraft_trace->profile_shape == aircraft_trace::shape::trapezoid)
            {
                required_distances_nm.push_back(route_distance_nm - aircraft_trace->descent_distance_nm);
            }
        }
        std::sort(required_distances_nm.begin(), required_distances_nm.end());
        required_distances_nm.erase(std::unique(required_distances_nm.begin(), required_distances_nm.end()),
                                    required_distances_nm.end());

        terrain_profile profile;
        std::vector<profile_station> stations{{0.0, {departure.lat, departure.lon}, departure_elevation_ft}};
        double leg_start_nm = 0.0;
        for(std::size_t leg_index = 0; leg_index + 1 < waypoints.size(); ++leg_index)
        {
            const auto& from = waypoints[leg_index];
            const auto& to = waypoints[leg_index + 1];
            const double leg_distance_nm = haversine_distance_nm(from.lat, from.lon, to.lat, to.lon);
            const double leg_end_nm = leg_start_nm + leg_distance_nm;
            const airspace_point from_point = stations.back().point;
            const airspace_point to_point{to.lat, unwrap_longitude(to.lon, from_point.lon)};
            const leg_stations leg{from_point, to_point, leg_start_nm, leg_distance_nm, min_interval_nm, terrain};
            const profile_station leg_end{leg_end_nm, to_point, elevation_at(&to, to.lat, to.lon, terrain, airports)};

            std::vector<double> piece_ends_nm;
            for(const double distance_nm : required_distances_nm)
            {
                if(distance_nm > leg_start_nm && distance_nm < leg_end_nm)
                {
                    piece_ends_nm.push_back(distance_nm);
                }
            }
            piece_ends_nm.push_back(leg_end_nm);
            for(const double piece_end_nm : piece_ends_nm)
            {
                const double piece_start_nm = stations.back().distance_nm;
                const auto seeds =
                    std::max(1, static_cast<int>(std::ceil((piece_end_nm - piece_start_nm) / max_interval_nm)));
                for(int seed = 1; seed <= seeds; ++seed)
                {
                    const auto start = stations.back();
                    const double end_nm =
                        seed == seeds ? piece_end_nm : piece_start_nm + (piece_end_nm - piece_start_nm) * seed / seeds;
                    leg.refine(start, end_nm == leg_end_nm ? leg_end : leg.at(end_nm), stations);
                }
            }

            profile.legs.push_back({leg_index, leg_start_nm, leg_end_nm, std::nullopt, std::nullopt, false});
            leg_start_nm = leg_end_nm;
        }

        const auto corridor_half_width_at = [&](double distance_nm)
        {
            const auto distance_from_terminal_nm = std::min(distance_nm, route_distance_nm - distance_nm);
            return corridor_half_width_nm *
                   std::clamp(distance_from_terminal_nm / TERRAIN_PROFILE_TERMINAL_CORRIDOR_DISTANCE_NM, 0.0, 1.0);
        };

        // Each interval's terrain and obstacle maxima cover the corridor
        // swept between its two stations, and each station takes the larger
        // of its two intervals, so a line between adjacent stations never
        // falls below the terrain or obstacles anywhere in the corridor
        // between them.
        std::vector<geo_bbox> interval_windows;
        std::vector<std::optional<double>> interval_maxima_ft;
        for(std::size_t station_index = 0; station_index + 1 < stations.size(); ++station_index)
        {
            const auto& start = stations[station_index];
            const auto& end = stations[station_index + 1];
            const geo_bbox window =
                bbox_union(bbox_around(start.point.lat, start.point.lon, corridor_half_width_at(start.distance_nm)),
                           bbox_around(end.point.lat, end.point.lon, corridor_half_width_at(end.distance_nm)));
            interval_windows.push_back(window);
            interval_maxima_ft.push_back(
                terrain.maximum_elevation_ft(window.lat_min, window.lon_min, window.lat_max, window.lon_max));
        }

        // Obstacles are fetched for runs of consecutive intervals and
        // assigned to each interval whose window contains them.
        std::vector<std::optional<double>> interval_obstacle_maxima_ft(interval_windows.size());
        for(std::size_t first = 0; include_obstacles && first < interval_windows.size();)
        {
            geo_bbox run_window = interval_windows[first];
            std::size_t end = first + 1;
            while(end < interval_windows.size() &&
                  stations[end + 1].distance_nm - stations[first].distance_nm <= OBSTACLE_QUERY_SPAN_NM)
            {
                run_window = bbox_union(run_window, interval_windows[end]);
                ++end;
            }
            for(const auto& obstacle : obstacles_in_bbox(airports, run_window))
            {
                for(std::size_t interval = first; interval < end; ++interval)
                {
                    if(inside_bbox(obstacle, interval_windows[interval]))
                    {
                        interval_obstacle_maxima_ft[interval] =
                            optional_max(interval_obstacle_maxima_ft[interval], static_cast<double>(obstacle.amsl_ht));
                    }
                }
            }
            first = end;
        }

        for(std::size_t station_index = 0; station_index < stations.size(); ++station_index)
        {
            const auto& station = stations[station_index];
            profile.samples.push_back(
                {station.distance_nm, station.elevation_ft, std::nullopt, std::nullopt, false, false});
            auto& sample = profile.samples.back();
            if(corridor_half_width_at(station.distance_nm) == 0.0)
            {
                sample.corridor_elevation_ft = station.elevation_ft;
                continue;
            }

            const bool has_previous = station_index > 0;
            const bool has_next = station_index + 1 < stations.size();
            sample.corridor_elevation_ft =
                optional_max(has_previous ? interval_maxima_ft[station_index - 1] : std::nullopt,
                             has_next ? interval_maxima_ft[station_index] : std::nullopt);
            const auto obstacle_maximum_ft =
                optional_max(has_previous ? interval_obstacle_maxima_ft[station_index - 1] : std::nullopt,
                             has_next ? interval_obstacle_maxima_ft[station_index] : std::nullopt);
            if(obstacle_maximum_ft)
            {
                sample.has_obstacle = true;
                sample.corridor_from_obstacle = !sample.corridor_elevation_ft;
                sample.corridor_elevation_ft = optional_max(sample.corridor_elevation_ft, obstacle_maximum_ft);
            }
        }

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
            std::optional<terrain_profile_clearance_severity> severity;
            if(phase && sample.aircraft_altitude_ft && sample.corridor_elevation_ft)
            {
                if(*sample.aircraft_altitude_ft < *sample.corridor_elevation_ft)
                {
                    severity = terrain_profile_clearance_severity::terrain_intersection;
                }
                else if(sample.distance_nm >= TERRAIN_PROFILE_TERMINAL_CORRIDOR_DISTANCE_NM &&
                        route_distance_nm - sample.distance_nm >= TERRAIN_PROFILE_TERMINAL_CORRIDOR_DISTANCE_NM &&
                        *sample.aircraft_altitude_ft < *sample.corridor_elevation_ft + required_clearance_ft)
                {
                    severity = terrain_profile_clearance_severity::below_clearance;
                }
            }
            if(severity && (!span || (span->phase == *phase && span->severity == *severity)))
            {
                if(!span)
                {
                    span = {sample.distance_nm, sample.distance_nm, *phase, *severity};
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
            if(severity)
            {
                span = {sample.distance_nm, sample.distance_nm, *phase, *severity};
            }
        }
        if(span)
        {
            profile.clearance_spans.push_back(*span);
        }
        return profile;
    }
} // namespace osect
