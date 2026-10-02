#include "route_submitter.hpp"
#include "flight_route.hpp"
#include "program.hpp"
#include "route_planner.hpp"
#include <atomic>
#include <thread>

namespace osect
{
    struct route_submitter::impl
    {
        route_planner planner;
        std::thread worker;
        std::atomic<bool> done{false};
        std::optional<planned_route> result;
        std::string error;
        std::uint64_t tag = 0;

        impl(const std::filesystem::path& db_path, const elevation_source& terrain) : planner(db_path, terrain)
        {
        }

        impl(const std::filesystem::path& db_path, const std::filesystem::path& user_db_path,
             const std::filesystem::path& ephemeral_db_path, const elevation_source& terrain)
            : planner(db_path, user_db_path, ephemeral_db_path, terrain)
        {
        }

        ~impl()
        {
            cancel_worker();
        }

        impl(const impl&) = delete;
        impl& operator=(const impl&) = delete;
        impl(impl&&) = delete;
        impl& operator=(impl&&) = delete;

        // Stops a running plan at its next A* step and joins it.
        void cancel_worker()
        {
            if(worker.joinable())
            {
                planner.request_cancel();
                worker.join();
                planner.clear_cancel();
            }
        }

        void submit(const std::string& text, const route_planner::options& opts, std::uint64_t tag)
        {
            cancel_worker();
            result.reset();
            error.clear();
            this->tag = tag;
            done = false;
            worker = std::thread(
                [this, text, opts]
                {
                    try
                    {
                        result.emplace(planner.parse(text, opts));
                    }
                    catch(const route_plan_cancelled&)
                    {
                        // The canceller joins this thread and discards
                        // its state.
                        return;
                    }
                    catch(const std::exception& e)
                    {
                        error = e.what();
                    }
                    done = true;
                    wake_main_thread();
                });
        }

        route_status poll()
        {
            if(!worker.joinable())
            {
                return {false, std::nullopt};
            }
            if(!done)
            {
                return {true, std::nullopt};
            }
            // Worker has finished. Join, transition to idle, and hand
            // the result/error to the caller in a single shot.
            worker.join();
            done = false;
            route_completion completion;
            if(result)
            {
                completion.route = std::move(result->route);
                completion.terrain_unchecked_nm = result->terrain_unchecked_nm;
            }
            completion.error = std::move(error);
            completion.tag = tag;
            result.reset();
            error.clear();
            return {false, std::move(completion)};
        }
    };

    route_submitter::route_submitter(const std::filesystem::path& db_path, const elevation_source& terrain)
        : pimpl(std::make_unique<impl>(db_path, terrain))
    {
    }

    route_submitter::route_submitter(const std::filesystem::path& db_path, const std::filesystem::path& user_db_path,
                                     const std::filesystem::path& ephemeral_db_path, const elevation_source& terrain)
        : pimpl(std::make_unique<impl>(db_path, user_db_path, ephemeral_db_path, terrain))
    {
    }

    route_submitter::~route_submitter() = default;

    void route_submitter::submit(const std::string& text, const route_planner::options& opts, std::uint64_t tag)
    {
        pimpl->submit(text, opts, tag);
    }

    route_status route_submitter::poll()
    {
        return pimpl->poll();
    }

} // namespace osect
