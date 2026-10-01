#include "terrain_profile_worker.hpp"
#include "elevation_source.hpp"
#include "nasr_database.hpp"
#include "program.hpp"
#include <atomic>
#include <thread>

namespace osect
{
    struct terrain_profile_worker::impl
    {
        const elevation_source& terrain;
        nasr_database airports;
        terrain_profile_margins margins;
        std::thread worker;
        std::atomic<bool> done{false};
        std::optional<terrain_profile> result;
        std::string error;

        impl(const elevation_source& terrain, const std::filesystem::path& db_path, terrain_profile_margins margins)
            : terrain(terrain), airports(db_path), margins(margins)
        {
        }

        impl(const impl&) = delete;
        impl& operator=(const impl&) = delete;
        impl(impl&&) = delete;
        impl& operator=(impl&&) = delete;

        ~impl()
        {
            if(worker.joinable())
            {
                worker.join();
            }
        }

        void submit(std::vector<route_waypoint> waypoints, std::optional<double> cruise_altitude_ft,
                    terrain_profile_gradients gradients)
        {
            if(worker.joinable())
            {
                worker.join();
            }
            result.reset();
            error.clear();
            done = false;
            worker = std::thread(
                [this, waypoints = std::move(waypoints), cruise_altitude_ft, gradients]
                {
                    try
                    {
                        result =
                            build_terrain_profile(waypoints, terrain, airports, cruise_altitude_ft, gradients, margins);
                    }
                    catch(const std::exception& e)
                    {
                        error = e.what();
                    }
                    done = true;
                    wake_main_thread();
                });
        }

        terrain_profile_status poll()
        {
            if(!worker.joinable())
            {
                return {};
            }
            if(!done)
            {
                return {true, std::nullopt, {}};
            }
            worker.join();
            done = false;
            auto status = terrain_profile_status{false, std::move(result), std::move(error)};
            result.reset();
            error.clear();
            return status;
        }
    };

    terrain_profile_worker::terrain_profile_worker(const elevation_source& terrain,
                                                   const std::filesystem::path& db_path,
                                                   terrain_profile_margins margins)
        : pimpl(std::make_unique<impl>(terrain, db_path, margins))
    {
    }

    terrain_profile_worker::~terrain_profile_worker() = default;

    void terrain_profile_worker::submit(std::vector<route_waypoint> waypoints, std::optional<double> cruise_altitude_ft,
                                        terrain_profile_gradients gradients)
    {
        pimpl->submit(std::move(waypoints), cruise_altitude_ft, gradients);
    }

    terrain_profile_status terrain_profile_worker::poll()
    {
        return pimpl->poll();
    }
} // namespace osect
