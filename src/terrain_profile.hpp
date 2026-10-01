#pragma once

#include <cstddef>
#include <optional>
#include <vector>

namespace osect
{
    class elevation_source;
    class nasr_database;
    struct route_waypoint;

    // Profile stations are spaced between these along-track distances.
    // An interval is subdivided until its conservative terrain maximum is
    // within the elevation tolerance of its lower endpoint, or until it
    // reaches the minimum interval.
    inline constexpr double TERRAIN_PROFILE_SAMPLE_INTERVAL_NM = 0.5;
    inline constexpr double TERRAIN_PROFILE_MAX_SAMPLE_INTERVAL_NM = 2.0;
    inline constexpr double TERRAIN_PROFILE_ELEVATION_TOLERANCE_FT = 100.0;
    inline constexpr std::size_t TERRAIN_PROFILE_MIN_SAMPLES = 100;
    inline constexpr double TERRAIN_PROFILE_CORRIDOR_HALF_WIDTH_NM = 4.0;
    inline constexpr double TERRAIN_PROFILE_TERMINAL_CORRIDOR_DISTANCE_NM = 10.0;
    inline constexpr double TERRAIN_PROFILE_REQUIRED_CLEARANCE_FT = 1000.0;

    // Along-track span of the consecutive station intervals whose
    // obstacles are fetched in one database query.
    inline constexpr double TERRAIN_PROFILE_OBSTACLE_QUERY_SPAN_NM = 20.0;

    enum class terrain_profile_phase
    {
        climb,
        cruise,
        descent,
    };

    enum class terrain_profile_clearance_severity
    {
        below_clearance,
        terrain_intersection,
    };

    struct terrain_profile_clearance_span
    {
        double start_distance_nm = 0.0;
        double end_distance_nm = 0.0;
        terrain_profile_phase phase = terrain_profile_phase::cruise;
        terrain_profile_clearance_severity severity = terrain_profile_clearance_severity::below_clearance;
    };

    // One point in a route's terrain and aircraft altitude traces.
    // Missing elevations represent unavailable terrain data or an
    // unspecified cruise altitude.
    struct terrain_profile_sample
    {
        double distance_nm = 0.0;
        std::optional<double> centreline_elevation_ft;
        std::optional<double> corridor_elevation_ft;
        std::optional<double> aircraft_altitude_ft;
        bool has_obstacle = false;
        bool corridor_from_obstacle = false;
    };

    // Derived terrain figures for one route leg.
    struct terrain_profile_leg
    {
        std::size_t route_leg_index = 0;
        double start_distance_nm = 0.0;
        double end_distance_nm = 0.0;
        std::optional<double> maximum_elevation_ft;
        std::optional<double> msa_ft;
        bool has_obstacle = false;
    };

    // Complete terrain result for a route.
    struct terrain_profile
    {
        std::vector<terrain_profile_sample> samples;
        std::vector<terrain_profile_leg> legs;
        std::optional<double> maximum_elevation_ft;
        std::vector<terrain_profile_clearance_span> clearance_spans;
    };

    // Fixed geometric climb and descent gradients, in feet per NM.
    struct terrain_profile_gradients
    {
        double climb_ft_per_nm = 300.0;
        double descent_ft_per_nm = 318.0;
    };

    // Samples terrain and the optional fixed-gradient aircraft trace along
    // route waypoints. Airport waypoints use their NASR field elevations;
    // all other points use the terrain source. Throws when an airport
    // waypoint is present but cannot be resolved in `airports`.
    terrain_profile build_terrain_profile(const std::vector<route_waypoint>& waypoints, const elevation_source& terrain,
                                          const nasr_database& airports, std::optional<double> cruise_altitude_ft,
                                          terrain_profile_gradients gradients = {},
                                          double corridor_half_width_nm = TERRAIN_PROFILE_CORRIDOR_HALF_WIDTH_NM,
                                          bool include_obstacles = true,
                                          double required_clearance_ft = TERRAIN_PROFILE_REQUIRED_CLEARANCE_FT);
} // namespace osect
