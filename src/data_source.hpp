#pragma once

#include <chrono>
#include <optional>
#include <string>

namespace osect
{
    // Per-source freshness assessment surfaced by the data-status panel.
    enum class data_source_status
    {
        fresh,   // now < expires
        expired, // now >= expires
        unknown, // no expires set; the registry has no opinion
    };

    // Plain value type. Populated by `nasr_database::list_data_sources()`
    // from the META table; consumed by `ui_overlay`. Pre-formatted
    // strings (`info`) carry everything the UI shows, so this struct
    // stays narrow.
    struct data_source
    {
        std::string name; // "nasr", "shp", "aixm", "dof", "adiz", "tfr"
        std::string info; // human description ("NASR cycle 16 Apr 2026")
        std::optional<std::chrono::system_clock::time_point> expires;

        data_source_status status() const;
        data_source_status status_at(std::chrono::system_clock::time_point now) const;
    };
}
