#pragma once

#include "route_planner.hpp"

class ini_config;

namespace nasrbrowse
{
    // Build a `route_planner::options` value from the [route_plan]
    // section of an ini file, applying g3xfplan-style defaults for
    // any missing key. The returned options have `use_airways =
    // false` — the GUI is responsible for turning that on. Throws
    // std::runtime_error if a preference value is not one of
    // PREFER / INCLUDE / AVOID / REJECT (case-insensitive).
    route_planner::options load_route_plan_options(const ini_config& ini);

} // namespace nasrbrowse
