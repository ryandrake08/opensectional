#pragma once

#include "nasr_database.hpp"
#include <variant>
#include <vector>

namespace nasrbrowse
{
    using pick_feature = std::variant<
        airport,
        navaid,
        fix,
        obstacle,
        class_airspace,
        sua,
        artcc,
        adiz,
        maa,
        pja,
        awos,
        comm_outlet,
        airway_segment,
        mtr_segment,
        runway
    >;

    struct pick_result
    {
        double lon;
        double lat;
        std::vector<pick_feature> features;
    };

} // namespace nasrbrowse
