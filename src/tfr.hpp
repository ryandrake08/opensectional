#pragma once

#include "geo_types.hpp"  // airspace_point
#include <string>
#include <vector>

namespace osect
{
    struct tfr_area
    {
        int area_id;
        std::string area_name;
        int upper_ft_val;
        std::string upper_ft_ref;
        int lower_ft_val;
        std::string lower_ft_ref;
        std::string date_effective;
        std::string date_expire;
        std::vector<airspace_point> points;
    };

    struct tfr
    {
        int tfr_id;
        std::string notam_id;
        std::string tfr_type;
        std::string facility;
        std::string date_effective;
        std::string date_expire;
        std::string description;
        std::vector<tfr_area> areas;
    };

    // Pre-tessellated rendering chunk. Built from tfr_area's polygon
    // by subdividing the ring into overlapping fixed-length spans;
    // see `subdivide_ring` (currently in tools/build_common.py and
    // soon to be ported as a small C++ helper).
    struct tfr_segment
    {
        int upper_ft_val;
        std::string upper_ft_ref;
        int lower_ft_val;
        std::string lower_ft_ref;
        std::vector<airspace_point> points;
    };
}
