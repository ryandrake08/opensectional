#pragma once

#include "data_source.hpp"
#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>

namespace osect
{
    // Background refresher for TFR data. Owns its own http_client
    // and a read-write ephemeral_database connection — the
    // database is the canonical store, and this class's job is
    // to keep it fresh by periodically fetching and parsing the
    // FAA TFR list and writing the result through. Every consumer
    // (feature_builder, the pick path) opens its own read-only
    // connection to the same file; there is no in-memory snapshot
    // shared across threads.
    //
    // Threading: the worker thread (owned here) is the only
    // writer to the database. The `last_updated` and
    // `as_data_source` accessors are safe from any thread —
    // `last_ok` is mutex-protected and `refresh_in_progress` is
    // atomic.
    class tfr_refresher
    {
        struct impl;
        std::unique_ptr<impl> pimpl;

    public:
        // Opens a read-write ephemeral_database at db_path and an
        // http_client (`offline` controls whether network fetches
        // are attempted), then starts the worker thread. Seeds
        // last_ok from the database's SOURCE_META so warm starts
        // and --offline runs report freshness without re-fetching.
        tfr_refresher(bool offline, const std::filesystem::path& db_path);
        ~tfr_refresher();

        tfr_refresher(const tfr_refresher&) = delete;
        tfr_refresher& operator=(const tfr_refresher&) = delete;

        // Pull the active TFR list, fetch each detail XML, write
        // the new state through the owned database connection,
        // update last_ok, and push an ephemeral_refresh event so
        // consumers re-query. Catches every libcurl / parse
        // failure so a single outage doesn't leave the app stuck;
        // on failure the database keeps its prior contents.
        //
        // Public for the test harness; production callers don't
        // invoke this directly — the worker thread does, on a
        // fixed interval.
        void refresh();

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
