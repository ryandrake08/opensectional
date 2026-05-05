#include "route_submitter.hpp"

#include "flight_route.hpp"  // route_parse_error
#include "route_planner.hpp"
#include "wake_main.hpp"

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

        explicit impl(const route_planner& p) : planner(p) {}
    };

    route_submitter::route_submitter(const route_planner& planner)
        : pimpl(std::make_unique<impl>(planner))
    {}

    route_submitter::~route_submitter()
    {
        if(pimpl->worker.joinable()) pimpl->worker.join();
    }

    void route_submitter::submit(const std::string& text,
                                  const route_planner::options& opts)
    {
        auto& d = *pimpl;
        if(d.worker.joinable()) d.worker.join();
        d.result.reset();
        d.error.clear();
        d.done = false;
        d.worker = std::thread([&d, text, opts] {
            try { d.result.emplace(d.planner.parse(text, opts)); }
            catch(const std::exception& e) { d.error = e.what(); }
            d.done = true;
            wake_main_thread();
        });
    }

    bool route_submitter::pending() const
    {
        return pimpl->worker.joinable() && !pimpl->done;
    }

    std::optional<flight_route> route_submitter::drain()
    {
        auto& d = *pimpl;
        if(!d.worker.joinable() || !d.done) return std::nullopt;
        d.worker.join();
        d.done = false;
        if(!d.error.empty())
            throw route_parse_error(d.error);
        return std::move(d.result);
    }

} // namespace osect
