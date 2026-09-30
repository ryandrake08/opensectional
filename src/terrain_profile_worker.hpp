#pragma once

#include "flight_route.hpp"
#include "terrain_profile.hpp"
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace osect
{
    class elevation_source;

    struct terrain_profile_status
    {
        bool pending = false;
        std::optional<terrain_profile> result;
        std::string error;
    };

    // Computes one route profile at a time on a worker thread. New requests
    // replace an undrained completed request.
    class terrain_profile_worker
    {
        struct impl;
        std::unique_ptr<impl> pimpl;

    public:
        terrain_profile_worker(const elevation_source& terrain, const std::filesystem::path& db_path);
        ~terrain_profile_worker();

        terrain_profile_worker(const terrain_profile_worker&) = delete;
        terrain_profile_worker& operator=(const terrain_profile_worker&) = delete;

        void submit(std::vector<route_waypoint> waypoints, std::optional<double> cruise_altitude_ft,
                    terrain_profile_gradients gradients);
        terrain_profile_status poll();
    };
} // namespace osect
