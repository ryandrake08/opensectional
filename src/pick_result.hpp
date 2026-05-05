#pragma once

#include "nasr_database.hpp"
#include "tfr.hpp"
#include <variant>
#include <vector>

namespace osect
{
    using feature = std::variant<airport, navaid, fix, obstacle, class_airspace, sua, artcc, adiz, tfr, maa, pja, awos,
                                 comm_outlet, airway_segment, mtr_segment, runway>;

    struct pick_result
    {
        double lon;
        double lat;
        std::vector<feature> features;
    };

} // namespace osect
