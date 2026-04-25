#pragma once

#include "route_planner.hpp"

#include <memory>
#include <optional>
#include <string>

namespace nasrbrowse
{
    // Async wrapper around route_planner::expand_sigils. Runs sigil
    // expansion on a background thread so the UI can show a progress
    // indicator during long plans. The UI polls for completion.
    class route_submitter
    {
        struct impl;
        std::unique_ptr<impl> pimpl;

    public:
        explicit route_submitter(const route_planner& planner);
        ~route_submitter();

        route_submitter(const route_submitter&) = delete;
        route_submitter& operator=(const route_submitter&) = delete;

        // Begin expanding `text` on a background thread. Any prior
        // submission that hasn't been drained is joined and dropped.
        // `opts` is captured by value so the caller can mutate or
        // destroy its copy immediately.
        void submit(const std::string& text,
                     const route_planner::options& opts);

        // True while a background expansion is in progress.
        bool pending() const;

        // Returns the sigil-free expanded text once the most recent
        // submit() completes, then transitions back to idle. Returns
        // nullopt while the expansion is pending or when no
        // submission is active. Throws route_parse_error if
        // expansion failed.
        std::optional<std::string> drain();
    };

} // namespace nasrbrowse
