#pragma once

#include "data_source.hpp"
#include <memory>
#include <vector>

namespace osect
{
    class tfr_source;

    // Single owner of every ephemeral data source the app pulls from
    // the network. Constructed once in main; passed by reference to
    // map_widget / feature_renderer / feature_builder / pick paths
    // in place of plumbing each individual *_source through every
    // layer. Adding a future source (NOTAMs, weather, ...) is one
    // new member, one accessor, one row in as_data_sources(), and one
    // tracker.add() in the constructor — none of the call sites change.
    //
    // Owns the shared http_client, the on-disk ephemeral_cache, and
    // every *_source that consumes them. The facade is closed: the
    // raw http/cache handles are not exposed, so consumers can't
    // reach around it for ad-hoc fetches.
    class ephemeral_data
    {
        struct impl;
        std::unique_ptr<impl> pimpl;

    public:
        // `offline=true` skips all network fetches and serves whatever
        // was last persisted in the cache. Construction is non-blocking;
        // background refreshes start on their own threads.
        explicit ephemeral_data(bool offline);
        ~ephemeral_data();

        ephemeral_data(const ephemeral_data&) = delete;
        ephemeral_data& operator=(const ephemeral_data&) = delete;

        const tfr_source& tfrs() const;

        // Per-source freshness snapshot for the data-status panel.
        std::vector<data_source> as_data_sources() const;

        // True when any owned source's last_updated() has advanced
        // since the last poll. The render loop uses this to
        // invalidate the cached feature build so a swapped-in fresh
        // dataset appears on screen without waiting for a pan/zoom.
        bool poll_advance();
    };
}
