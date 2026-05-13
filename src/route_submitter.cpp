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
        std::optional<flight_route> result;
        std::string error;
        std::uint64_t tag = 0;

        explicit impl(const char* db_path) : planner(db_path)
        {
        }

        ~impl()
        {
            if(worker.joinable())
            {
                worker.join();
            }
        }

        impl(const impl&) = delete;
        impl& operator=(const impl&) = delete;
        impl(impl&&) = delete;
        impl& operator=(impl&&) = delete;

        void submit(const std::string& text, const route_planner::options& opts, std::uint64_t tag)
        {
            if(worker.joinable())
            {
                worker.join();
            }
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
            completion.route = std::move(result);
            completion.error = std::move(error);
            completion.tag = tag;
            result.reset();
            error.clear();
            return {false, std::move(completion)};
        }
    };

    route_submitter::route_submitter(const char* db_path) : pimpl(std::make_unique<impl>(db_path))
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
