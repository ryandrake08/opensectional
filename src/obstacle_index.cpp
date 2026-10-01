#include "obstacle_index.hpp"
#include "nasr_database.hpp"
#include <algorithm>
#include <cmath>

namespace
{
    constexpr int LAT_CELLS = 180;
    constexpr int LON_CELLS = 360;

    std::size_t cell_id(int lat_index, int lon_index)
    {
        return static_cast<std::size_t>(lat_index) * LON_CELLS + static_cast<std::size_t>(lon_index);
    }
}

namespace osect
{
    obstacle_index::obstacle_index(const nasr_database& db)
        : db(db),
          cells(static_cast<std::size_t>(LAT_CELLS) * LON_CELLS),
          loaded(static_cast<std::size_t>(LAT_CELLS) * LON_CELLS, 0)
    {
    }

    const std::vector<obstacle_height>& obstacle_index::cell(int lat_index, int lon_index)
    {
        const auto id = cell_id(lat_index, lon_index);
        if(!loaded[id])
        {
            const double lat = lat_index - 90.0;
            const double lon = lon_index - 180.0;
            auto& obstacles = cells[id];
            obstacles = db.query_obstacle_heights({lon, lat, lon + 1.0, lat + 1.0});
            std::sort(obstacles.begin(), obstacles.end(),
                      [](const obstacle_height& a, const obstacle_height& b) { return a.amsl_ht > b.amsl_ht; });
            loaded[id] = 1;
        }
        return cells[id];
    }

    std::optional<double> obstacle_index::maximum_ft(const geo_bbox& box)
    {
        std::optional<double> maximum_ft;
        const int first_lat = std::max(0, static_cast<int>(std::floor(box.lat_min + 90.0)));
        const int last_lat = std::min(LAT_CELLS - 1, static_cast<int>(std::floor(box.lat_max + 90.0)));
        const auto first_lon = static_cast<int>(std::floor(box.lon_min));
        const auto last_lon = std::min(first_lon + LON_CELLS - 1, static_cast<int>(std::floor(box.lon_max)));
        for(int lat_index = first_lat; lat_index <= last_lat; ++lat_index)
        {
            for(int lon_deg = first_lon; lon_deg <= last_lon; ++lon_deg)
            {
                // The cell holding longitude `lon_deg` after wrapping, and
                // the whole turns that shift its obstacles into the box.
                const int wrapped = ((lon_deg + 180) % 360 + 360) % 360;
                const double shift = lon_deg - (wrapped - 180.0);
                for(const auto& obstacle : cell(lat_index, wrapped))
                {
                    if(maximum_ft && obstacle.amsl_ht <= *maximum_ft)
                    {
                        break;
                    }
                    const double lon = obstacle.lon + shift;
                    if(obstacle.lat >= box.lat_min && obstacle.lat <= box.lat_max && lon >= box.lon_min &&
                       lon <= box.lon_max)
                    {
                        maximum_ft = obstacle.amsl_ht;
                        break;
                    }
                }
            }
        }
        return maximum_ft;
    }
} // namespace osect
