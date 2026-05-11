#pragma once

#include "geo_types.hpp" // airspace_point
#include <string>
#include <vector>

namespace osect
{
    struct tfr_area
    {
        int area_id;
        std::string area_name;
        int upper_ft_val;
        std::string upper_ft_ref; // "MSL" / "SFC" / "AGL" / "STD" / "OTHER"
        int lower_ft_val;
        std::string lower_ft_ref; // "MSL" / "SFC" / "AGL" / "STD" / "OTHER"
        std::string date_effective;
        std::string date_expire;
        std::string start_time;        // daily window start, HHMM
        std::string end_time;          // daily window end, HHMM
        std::string is_time_separate;  // "TRUE" / "FALSE" — daily-recurring flag
        std::string day_code;          // days-of-week mask (FAA encoding)
        std::string instructions; // multiple <txtInstr> joined with '\n'
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
        std::string date_issued;
        std::string city;
        std::string state;
        std::string coord_facility;
        std::string coord_facility_name;
        std::string coord_facility_type;
        std::string coord_phone;
        std::string coord_freq;
        std::string poc_name;
        std::string poc_org;
        std::string poc_phone;
        std::string poc_freq;
        std::string time_zone;        // e.g. "EDT" — context for date_effective / date_expire
        std::string expire_time_zone; // e.g. "EDT" — context for date_expire when it differs
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
