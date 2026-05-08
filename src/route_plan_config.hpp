#pragma once

#include "route_planner.hpp"
#include <string>

class ini_config;

namespace osect
{
    // Build a `route_planner::options` value from the [route_plan]
    // section of an ini file, applying g3xfplan-style defaults for
    // any missing key. The returned options have `use_airways =
    // false` — the GUI is responsible for turning that on. Throws
    // std::runtime_error if a preference value is not one of
    // PREFER / INCLUDE / AVOID / REJECT (case-insensitive), or if
    // validate_route_plan_options rejects the loaded values.
    route_planner::options load_route_plan_options(const ini_config& ini);

    // Check a populated route_plan_options for obviously bad values
    // (e.g. non-positive max_leg_length_nm, which makes the planner
    // accept no edges and produce a degenerate plan). Returns an
    // empty string when valid; otherwise an error message naming the
    // offending field, suitable for throwing at .ini load time or
    // displaying in the GUI route panel before submission.
    std::string validate_route_plan_options(const route_plan_options& opts);

} // namespace osect
