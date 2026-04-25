#include "route_plan_config.hpp"

#include "ini_config.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace nasrbrowse
{
    namespace
    {
        std::string upper(std::string s)
        {
            std::transform(s.begin(), s.end(), s.begin(),
                [](unsigned char c) { return std::toupper(c); });
            return s;
        }

        // Translate a PREFER / INCLUDE / AVOID / REJECT string
        // into the numeric cost modifier. Throws on unknown.
        double parse_pref(const std::string& key, const std::string& value)
        {
            auto v = upper(value);
            if(v == "PREFER")  return cost_prefer;
            if(v == "INCLUDE") return cost_include;
            if(v == "AVOID")   return cost_avoid;
            if(v == "REJECT")  return cost_reject;
            throw std::runtime_error(
                "route_plan: unknown preference '" + value +
                "' for key '" + key +
                "' (expected PREFER / INCLUDE / AVOID / REJECT)");
        }

        // Helper: read a preference key with a default. The default
        // applies when the key is absent from the ini.
        double pref_or(const ini_config& ini,
                        const std::string& key, double fallback)
        {
            if(!ini.exists(key)) return fallback;
            return parse_pref(key, ini.get<std::string>(key));
        }
    }

    route_planner::options load_route_plan_options(const ini_config& ini)
    {
        route_planner::options o;
        using ws = wp_subtype;
        using ac = awy_class;

        // Max leg length. Default matches g3xfplan.
        o.max_leg_length_nm = ini.exists("route_plan.max_leg_length_nm")
            ? ini.get<double>("route_plan.max_leg_length_nm")
            : 80.0;

        // Waypoint subtypes. Defaults match g3xfplan's argparse
        // defaults: airports INCLUDE, other airport flavors REJECT,
        // navaids REJECT (overridden to INCLUDE by the use-airways
        // toggle), VFR fix INCLUDE.
        const auto INCL = cost_include;
        const auto REJ  = cost_reject;

        o.wp_cost[static_cast<std::size_t>(ws::airport_landplane)] =
            pref_or(ini, "route_plan.route_waypoint_airport", INCL);
        o.wp_cost[static_cast<std::size_t>(ws::airport_balloonport)] =
            pref_or(ini, "route_plan.route_waypoint_balloonport", REJ);
        o.wp_cost[static_cast<std::size_t>(ws::airport_seaplane)] =
            pref_or(ini, "route_plan.route_waypoint_seaplane_base", REJ);
        o.wp_cost[static_cast<std::size_t>(ws::airport_gliderport)] =
            pref_or(ini, "route_plan.route_waypoint_gliderport", REJ);
        o.wp_cost[static_cast<std::size_t>(ws::airport_heliport)] =
            pref_or(ini, "route_plan.route_waypoint_heliport", REJ);
        o.wp_cost[static_cast<std::size_t>(ws::airport_ultralight)] =
            pref_or(ini, "route_plan.route_waypoint_ultralight", REJ);

        o.wp_cost[static_cast<std::size_t>(ws::navaid_vor)] =
            pref_or(ini, "route_plan.route_waypoint_vor", REJ);
        o.wp_cost[static_cast<std::size_t>(ws::navaid_vortac)] =
            pref_or(ini, "route_plan.route_waypoint_vortac", REJ);
        o.wp_cost[static_cast<std::size_t>(ws::navaid_vor_dme)] =
            pref_or(ini, "route_plan.route_waypoint_vordme", REJ);
        o.wp_cost[static_cast<std::size_t>(ws::navaid_dme)] =
            pref_or(ini, "route_plan.route_waypoint_dme", REJ);
        o.wp_cost[static_cast<std::size_t>(ws::navaid_ndb)] =
            pref_or(ini, "route_plan.route_waypoint_ndb", REJ);
        o.wp_cost[static_cast<std::size_t>(ws::navaid_ndb_dme)] =
            pref_or(ini, "route_plan.route_waypoint_ndbdme", REJ);

        // WP/RP/CN/MR are not user-configurable: they're REJECT
        // when use_airways is off, INCLUDE (via override) when on.
        o.wp_cost[static_cast<std::size_t>(ws::fix_wp)] = REJ;
        o.wp_cost[static_cast<std::size_t>(ws::fix_rp)] = REJ;
        o.wp_cost[static_cast<std::size_t>(ws::fix_cn)] = REJ;
        o.wp_cost[static_cast<std::size_t>(ws::fix_mr)] = REJ;

        o.wp_cost[static_cast<std::size_t>(ws::fix_vfr)] =
            pref_or(ini, "route_plan.route_waypoint_vfr", INCL);

        // Airway classes. g3xfplan defaults: Victor PREFER, RNAV
        // INCLUDE, Jet/colored/other REJECT.
        o.awy_cost[static_cast<std::size_t>(ac::victor)] =
            pref_or(ini, "route_plan.route_airway_victor",
                    cost_prefer);
        o.awy_cost[static_cast<std::size_t>(ac::jet)] =
            pref_or(ini, "route_plan.route_airway_jet", REJ);
        o.awy_cost[static_cast<std::size_t>(ac::rnav)] =
            pref_or(ini, "route_plan.route_airway_rnav", INCL);
        o.awy_cost[static_cast<std::size_t>(ac::color)] =
            pref_or(ini, "route_plan.route_airway_color", REJ);
        o.awy_cost[static_cast<std::size_t>(ac::other)] =
            pref_or(ini, "route_plan.route_airway_other", REJ);

        // Cost factor for airway edges marked as published gaps.
        // Default INCLUDE — neutral, treats the bridge as a regular
        // direct hop while preserving the airway-ID metadata for
        // shorthand collapse.
        o.gap_cost = pref_or(ini, "route_plan.route_airway_gap", INCL);

        // The GUI controls use_airways at runtime — leave it off
        // by default so a freshly-loaded ini reproduces the
        // previous (uniform) behavior.
        o.use_airways = false;
        return o;
    }

} // namespace nasrbrowse
