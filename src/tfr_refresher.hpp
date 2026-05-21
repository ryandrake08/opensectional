#pragma once

#include <filesystem>
#include <memory>

namespace osect
{

    // Background refresher for TFR data. Owns its own http_client
    // and an ephemeral_database connection — the database is the
    // canonical store, and this class's job is to keep it fresh by
    // periodically fetching the FAA TFR list and writing the result
    // through. Every consumer (feature_builder, the pick path) opens
    // its own connection to the same file, held const so it only
    // reads.
    //
    // Construct only when online; --offline skips it entirely and
    // reads freshness from the database directly.
    class tfr_refresher
    {
        struct impl;
        std::unique_ptr<impl> pimpl;

    public:
        explicit tfr_refresher(const std::filesystem::path& db_path);
        ~tfr_refresher();

        tfr_refresher(const tfr_refresher&) = delete;
        tfr_refresher& operator=(const tfr_refresher&) = delete;

        // True while a refresh is running. Drives the panel's UPD
        // indicator.
        bool is_refreshing() const;
    };
}
