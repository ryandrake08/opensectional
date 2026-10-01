#pragma once

#include "geo_types.hpp"
#include "nasr_database.hpp"
#include <cstdint>
#include <optional>
#include <vector>

namespace osect
{
    // DOF obstacle heights in 1-degree cells, each read from the database
    // on the first lookup that touches it and kept tallest first. Not
    // thread-safe: one owner thread does every lookup.
    class obstacle_index
    {
        const nasr_database& db;
        std::vector<std::vector<obstacle_height>> cells;
        std::vector<std::uint8_t> loaded;

        const std::vector<obstacle_height>& cell(int lat_index, int lon_index);

    public:
        // `db` must outlive the index.
        explicit obstacle_index(const nasr_database& db);

        // The highest obstacle AMSL, in feet, inside `box`, or nullopt
        // when it holds none. Longitudes may lie outside [-180, 180];
        // an obstacle is inside when some whole-turn shift of its
        // longitude is.
        std::optional<double> maximum_ft(const geo_bbox& box);
    };
} // namespace osect
