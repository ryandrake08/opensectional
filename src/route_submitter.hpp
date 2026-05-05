#pragma once

#include "flight_route.hpp"
#include "route_planner.hpp"

#include <memory>
#include <optional>
#include <string>

namespace osect
{
    // Async wrapper around route_planner::expand_sigils + flight_route
    // construction. Both run on a background thread so the UI can show
    // a progress indicator during long plans, and the main thread
    // never has to do per-token SQLite lookups synchronously after a
    // route comes back.
    class route_submitter
    {
        struct impl;
        std::unique_ptr<impl> pimpl;

    public:
        // The planner does the A* and the route parse via
        // route_planner::parse(). The planner's database handle is
        // distinct from the rendering thread's so the worker doesn't
        // contend on its mutex. The planner must outlive the submitter.
        explicit route_submitter(const route_planner& planner);
        ~route_submitter();

        route_submitter(const route_submitter&) = delete;
        route_submitter& operator=(const route_submitter&) = delete;

        // Begin expanding `text` on a background thread. Any prior
        // submission that hasn't been drained is joined and dropped.
        // `opts` is captured by value so the caller can mutate or
        // destroy its copy immediately. `text` must be non-empty;
        // callers handle the "clear route" case directly without
        // round-tripping through the worker.
        void submit(const std::string& text,
                     const route_planner::options& opts);

        // True while a background expansion is in progress.
        bool pending() const;

        // Returns the parsed route once the most recent submit()
        // completes, then transitions back to idle. Returns nullopt
        // while the expansion is pending or when no submission is
        // active. Throws route_parse_error on expansion or parse
        // failure.
        std::optional<flight_route> drain();
    };

} // namespace osect
