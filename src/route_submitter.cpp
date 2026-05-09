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
        const route_planner& planner;
        std::thread worker;
        std::atomic<bool> done{false};
        std::optional<flight_route> result;
        std::string error;
        std::uint64_t tag = 0;

        explicit impl(const route_planner& p) : planner(p)
        {
        }
    };

    route_submitter::route_submitter(const route_planner& planner) : pimpl(std::make_unique<impl>(planner))
    {
    }

    route_submitter::~route_submitter()
    {
        if(pimpl->worker.joinable())
        {
            pimpl->worker.join();
        }
    }

    void route_submitter::submit(const std::string& text, const route_planner::options& opts, std::uint64_t tag)
    {
        auto& d = *pimpl;
        if(d.worker.joinable())
        {
            d.worker.join();
        }
        d.result.reset();
        d.error.clear();
        d.tag = tag;
        d.done = false;
        d.worker = std::thread(
            [&d, text, opts]
            {
                try
                {
                    d.result.emplace(d.planner.parse(text, opts));
                }
                catch(const std::exception& e)
                {
                    d.error = e.what();
                }
                d.done = true;
                wake_main_thread();
            });
    }

    route_status route_submitter::poll()
    {
        auto& d = *pimpl;
        if(!d.worker.joinable())
        {
            return {false, std::nullopt};
        }
        if(!d.done)
        {
            return {true, std::nullopt};
        }
        // Worker has finished. Join, transition to idle, and hand
        // the result/error to the caller in a single shot.
        d.worker.join();
        d.done = false;
        route_completion completion;
        completion.route = std::move(d.result);
        completion.error = std::move(d.error);
        completion.tag = d.tag;
        d.result.reset();
        d.error.clear();
        return {false, std::move(completion)};
    }

} // namespace osect
