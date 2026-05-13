#pragma once

#include "flight_route.hpp"
#include "route_planner.hpp"
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace osect
{
    // Outcome of a completed submission. Exactly one of `route` or
    // `error` is populated when this is delivered via route_status.
    // `tag` echoes back the value passed to submit(), letting the
    // caller route the result to the originating logical owner (e.g.
    // a route panel tab) even if the user has shifted focus since.
    struct route_completion
    {
        // Set on success.
        std::optional<flight_route> route;
        // Set on failure: the message from route_parse_error::what()
        // (or any other std::exception thrown by the parser).
        std::string error;
        std::uint64_t tag = 0;
    };

    // Snapshot of the submitter's state, returned by poll(). `pending`
    // and `completion` are mutually exclusive: a completed submission
    // is delivered with `pending == false`, so callers never have to
    // reason about an in-flight plan and a finished plan at the same
    // time.
    struct route_status
    {
        bool pending = false;
        // Populated for exactly one poll() call — the one that
        // observes the worker has finished. Subsequent polls return
        // `pending == false, completion == nullopt` (idle).
        std::optional<route_completion> completion;
    };

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
        // Constructs an internal route_planner against `db_path`. The
        // planner's database handle is distinct from the rendering
        // thread's so the worker doesn't contend on its mutex.
        explicit route_submitter(const char* db_path);
        ~route_submitter();

        route_submitter(const route_submitter&) = delete;
        route_submitter& operator=(const route_submitter&) = delete;

        // Begin expanding `text` on a background thread. Any prior
        // submission that hasn't been drained is joined and dropped.
        // `opts` is captured by value so the caller can mutate or
        // destroy its copy immediately. `text` must be non-empty;
        // callers handle the "clear route" case directly without
        // round-tripping through the worker. `tag` is echoed back
        // verbatim in the resulting route_completion.
        void submit(const std::string& text, const route_planner::options& opts, std::uint64_t tag);

        // Single per-frame status read. Resolves the worker's state
        // exactly once so callers can't observe `pending` and a
        // completion in the same frame. Transitions ready→idle on
        // the call that delivers the completion.
        route_status poll();
    };

} // namespace osect
