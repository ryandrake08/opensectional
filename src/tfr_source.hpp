#pragma once

#include "data_source.hpp"
#include "tfr.hpp"
#include <chrono>
#include <memory>
#include <optional>
#include <vector>

namespace osect
{
    class http_client;
    class ephemeral_cache;

    // In-memory store of currently-active TFRs, with a network refresh
    // path that tolerates being offline. Replaces the old SQLite-backed
    // TFR_* tables for the consuming render path.
    //
    // Threading: refresh() is intended to run on a background thread.
    // snapshot() / snapshot_segments() / last_updated() are safe from
    // any thread; they take a shared lock and copy state out so the
    // caller can hold the result independently of the next refresh.
    class tfr_source
    {
        struct impl;
        std::unique_ptr<impl> pimpl;

    public:
        // On construction the source loads any prior state from the
        // cache so warm starts and `--offline` runs see whatever was
        // last successfully fetched. Construction never throws on a
        // missing or malformed cache — it just leaves the in-memory
        // store empty.
        tfr_source(http_client& http, ephemeral_cache& cache);
        ~tfr_source();

        tfr_source(const tfr_source&) = delete;
        tfr_source& operator=(const tfr_source&) = delete;

        // Pull the active TFR list, fetch each detail XML, replace the
        // in-memory store atomically, and persist the new state to the
        // cache. Catches every libcurl / parse failure so a single
        // outage doesn't leave the app stuck. On full failure the
        // existing in-memory store is preserved.
        void refresh();

        // Copy of the current TFR list (most callers iterate once and
        // discard, so a copy is cheap and saves us a lock-aware view).
        std::vector<tfr> snapshot() const;

        // Pre-tessellated rendering segments. One per subdivide_ring
        // chunk per area. Stored alongside the parsed TFRs so the
        // render path doesn't recompute on every frame.
        std::vector<tfr_segment> snapshot_segments() const;

        // Wall-clock time of the most recent successful refresh
        // (or cache load). nullopt if neither has succeeded yet.
        std::optional<std::chrono::system_clock::time_point> last_updated() const;

        // Build a snapshot suitable for the data-source registry
        // panel. Status is computed from last_updated(): fresh
        // within 24 hours, expired beyond that, unknown if no
        // successful refresh or cache load has happened yet.
        data_source as_data_source() const;
    };
}
