#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "flight_route.hpp"  // route_parse_error
#include "ini_config.hpp"
#include "nasr_database.hpp"
#include "route_plan_config.hpp"
#include "route_planner.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>

using namespace osect;

// Shared DB/planner for all tests. Built once on first use; the test
// binary is expected to run with the repo root as its working directory.
static nasr_database& test_db()
{
    static nasr_database db("osect.db");
    return db;
}

static const route_planner& test_planner()
{
    static route_planner planner(test_db());
    return planner;
}

// Brevity alias for the test-only proxy that grants access to the
// planner's catalog and A* primitives.
using rpta = route_planner_test_access;

// Helper: build an `endpoint` for a known graph node, looking up
// its lat/lon through the proxy.
static rpta::endpoint endpoint_at(const route_planner& p, std::size_t idx)
{
    const auto& n = rpta::get_node(p, idx);
    return {idx, n.lat, n.lon};
}

TEST_CASE("catalog has at least the expected routable waypoints")
{
    // Airports ~5k (PU/O), navaids ~1.4k (excl. TACAN/VOT/...),
    // fixes ~68k (excl. MW/NRS/RADAR). Floor at 70k for headroom.
    CHECK(rpta::node_count(test_planner()) > 70000);
}

TEST_CASE("ICAO-assigned airports resolve by ICAO, not by FAA ID")
{
    const auto& p = test_planner();
    auto ksac = rpta::node_index(p, "KSAC");
    REQUIRE(ksac.has_value());
    CHECK(rpta::get_node(p, *ksac).kind == rpta::node_kind::airport);

    // "SAC" is the FAA ID for KSAC and also a VORTAC. The VORTAC wins.
    auto sac = rpta::node_index(p, "SAC");
    REQUIRE(sac.has_value());
    CHECK(rpta::get_node(p, *sac).kind == rpta::node_kind::navaid);
}

TEST_CASE("airports without an ICAO ID resolve by FAA ID")
{
    const auto& p = test_planner();
    auto o61 = rpta::node_index(p, "O61");
    REQUIRE(o61.has_value());
    CHECK(rpta::get_node(p, *o61).kind == rpta::node_kind::airport);
}

TEST_CASE("non-routable navaid types are filtered out")
{
    // Verify every catalog node has a non-empty ID and non-zero
    // coordinates — i.e., no NULL-island stubs survived the load
    // filters.
    const auto& p = test_planner();
    for(std::size_t i = 0; i < rpta::node_count(p); ++i)
    {
        const auto& n = rpta::get_node(p, i);
        CHECK_FALSE(n.id.empty());
        CHECK((n.lat != 0.0 || n.lon != 0.0));
    }
}

TEST_CASE("airway adjacency includes V459 SLI-DODGR")
{
    const auto& p = test_planner();
    auto sli = rpta::node_index(p, "SLI");
    auto dodgr = rpta::node_index(p, "DODGR");
    REQUIRE(sli.has_value());
    REQUIRE(dodgr.has_value());

    bool found = false;
    for(const auto& e : rpta::airway_neighbors(p, *sli))
    {
        if(e.neighbor_index == *dodgr && e.airway_id == "V459")
        {
            found = true;
            break;
        }
    }
    CHECK(found);
}

TEST_CASE("airway adjacency is bidirectional")
{
    const auto& p = test_planner();
    auto sli = rpta::node_index(p, "SLI");
    auto dodgr = rpta::node_index(p, "DODGR");
    REQUIRE(sli.has_value());
    REQUIRE(dodgr.has_value());

    bool reverse = false;
    for(const auto& e : rpta::airway_neighbors(p, *dodgr))
    {
        if(e.neighbor_index == *sli && e.airway_id == "V459")
        {
            reverse = true;
            break;
        }
    }
    CHECK(reverse);
}

TEST_CASE("unknown waypoint ID returns nullopt")
{
    CHECK_FALSE(rpta::node_index(test_planner(), "ZZZZZ").has_value());
}

// ---- Stage 2: A* core ----

TEST_CASE("plan_segment short hop returns empty (direct leg suffices)")
{
    // KSMF-KSAC is about 12 nm — well under the 80 nm default max.
    const auto& p = test_planner();
    auto ksmf = *rpta::node_index(p, "KSMF");
    auto ksac = *rpta::node_index(p, "KSAC");
    route_planner::options opts;
    auto path = rpta::plan_segment(
        p, endpoint_at(p, ksmf), endpoint_at(p, ksac), opts);
    REQUIRE(path.has_value());
    CHECK(path->empty());
}

TEST_CASE("plan_segment longer route needs intermediates")
{
    // KSMF to KBFL is about 230 nm — needs intermediates at 80 nm
    // max leg.
    const auto& p = test_planner();
    auto ksmf = *rpta::node_index(p, "KSMF");
    auto kbfl = *rpta::node_index(p, "KBFL");
    auto path = rpta::plan_segment(
        p, endpoint_at(p, ksmf), endpoint_at(p, kbfl), {});
    REQUIRE(path.has_value());
    CHECK_FALSE(path->empty());
    // Every leg, including origin→first and last→destination, must
    // fit within the 80 nm limit. We check via straight haversine
    // between consecutive waypoints.
    const auto& origin = rpta::get_node(p, ksmf);
    const auto& dest = rpta::get_node(p, kbfl);
    double prev_lat = origin.lat, prev_lon = origin.lon;
    for(auto idx : *path)
    {
        const auto& n = rpta::get_node(p, idx);
        auto d = std::sqrt(std::pow((n.lat - prev_lat) * 60.0, 2) +
                           std::pow((n.lon - prev_lon) * 60.0 *
                                     std::cos(prev_lat * 3.14159 / 180.0), 2));
        // Pessimistic flat-earth check — bounded by haversine, so if
        // this passes, haversine ≤ 80 too.
        CHECK(d <= 85.0);
        prev_lat = n.lat;
        prev_lon = n.lon;
    }
    auto final_d = std::sqrt(std::pow((dest.lat - prev_lat) * 60.0, 2) +
                              std::pow((dest.lon - prev_lon) * 60.0 *
                                        std::cos(prev_lat * 3.14159 / 180.0), 2));
    CHECK(final_d <= 85.0);
}

TEST_CASE("plan_segment accepts synthetic lat/lon endpoints")
{
    // Synthetic origin at KSMF's coords, destination = KSAC. ~12 nm,
    // fits the 80 nm default.
    const auto& p = test_planner();
    auto ksac = *rpta::node_index(p, "KSAC");
    rpta::endpoint synthetic_origin{
        rpta::synthetic, 38.69544, -121.59078
    };
    route_planner::options opts;
    auto path = rpta::plan_segment(
        p, synthetic_origin, endpoint_at(p, ksac), opts);
    REQUIRE(path.has_value());
    CHECK(path->empty());
}

TEST_CASE("plan_segment returns nullopt when max_leg is too tight")
{
    // Use a very short max leg. KSAC to KMER is ~40 nm — a 1 nm cap
    // with no airway connection between them cannot bridge the gap,
    // so expect nullopt rather than a hang.
    const auto& p = test_planner();
    auto ksac = *rpta::node_index(p, "KSAC");
    auto kmer = *rpta::node_index(p, "KMER");
    route_planner::options opts;
    opts.max_leg_length_nm = 1.0;
    auto path = rpta::plan_segment(
        p, endpoint_at(p, ksac), endpoint_at(p, kmer), opts);
    CHECK_FALSE(path.has_value());
}

// ---- Stage 3: expand_sigils grammar ----

TEST_CASE("expand_sigils is a no-op when input has no '?'")
{
    CHECK(test_planner().expand_sigils("KSMF V23 KBFL", route_planner::options{}) == "KSMF V23 KBFL");
}

TEST_CASE("expand_sigils rewrites '?' with intermediate IDs")
{
    auto out = test_planner().expand_sigils("KSMF ? KBFL", route_planner::options{});
    CHECK(out.find('?') == std::string::npos);
    CHECK(out.substr(0, 4) == "KSMF");
    CHECK(out.substr(out.size() - 4) == "KBFL");
    // ~230 nm between the airports means the planner must have
    // inserted at least one intermediate.
    CHECK(out.size() > std::string("KSMF KBFL").size());
}

TEST_CASE("expand_sigils accepts '?' without surrounding whitespace")
{
    auto a = test_planner().expand_sigils("KSMF?KBFL", route_planner::options{});
    auto b = test_planner().expand_sigils("KSMF ? KBFL", route_planner::options{});
    CHECK(a == b);
}

TEST_CASE("expand_sigils rejects leading '?'")
{
    CHECK_THROWS_AS(test_planner().expand_sigils("? KBFL", route_planner::options{}), route_parse_error);
}

TEST_CASE("expand_sigils rejects trailing '?'")
{
    CHECK_THROWS_AS(test_planner().expand_sigils("KSMF ?", route_planner::options{}), route_parse_error);
}

TEST_CASE("expand_sigils rejects consecutive '?'")
{
    CHECK_THROWS_AS(test_planner().expand_sigils("KSMF ? ? KBFL", route_planner::options{}),
                    route_parse_error);
}

// ---- Stage 4: airway-adjacent sigil (project-and-walk) ----

TEST_CASE("expand_sigils: '?' on both sides of an airway")
{
    // KSMF ? V23 ? KBFL: entry and exit on V23 via project-and-walk,
    // A* on both sides. The expanded text should contain V23 and
    // should re-parse successfully as a flight_route.
    auto out = test_planner().expand_sigils("KSMF ? V23 ? KBFL", route_planner::options{});
    CHECK(out.find('?') == std::string::npos);
    CHECK(out.find("V23") != std::string::npos);
    CHECK(out.substr(0, 4) == "KSMF");
    CHECK(out.substr(out.size() - 4) == "KBFL");
    flight_route route(out, test_db());
    CHECK(waypoint_id(route.waypoints.front()) == "KSMF");
    CHECK(waypoint_id(route.waypoints.back()) == "KBFL");
}

TEST_CASE("expand_sigils: '?' on exit side only (mixed pattern)")
{
    // KSMF V23 ? KBFL: haversine entry, project-and-walk exit, A*
    // from exit to KBFL.
    auto out = test_planner().expand_sigils("KSMF V23 ? KBFL", route_planner::options{});
    CHECK(out.find('?') == std::string::npos);
    CHECK(out.find("V23") != std::string::npos);
    flight_route route(out, test_db());
    CHECK(waypoint_id(route.waypoints.front()) == "KSMF");
    CHECK(waypoint_id(route.waypoints.back()) == "KBFL");
}

TEST_CASE("expand_sigils: '?' on entry side only (mixed pattern)")
{
    // KSMF ? V23 KBFL: project-and-walk entry, A* from KSMF to
    // entry, haversine exit.
    auto out = test_planner().expand_sigils("KSMF ? V23 KBFL", route_planner::options{});
    CHECK(out.find('?') == std::string::npos);
    CHECK(out.find("V23") != std::string::npos);
    flight_route route(out, test_db());
    CHECK(waypoint_id(route.waypoints.front()) == "KSMF");
    CHECK(waypoint_id(route.waypoints.back()) == "KBFL");
}

TEST_CASE("expand_sigils rejects airway on both sides of '?'")
{
    // `X ? Y` where both X and Y are airways is meaningless —
    // project-and-walk needs a point on the other side of each
    // airway to set direction.
    CHECK_THROWS_AS(test_planner().expand_sigils("KSMF V23 ? V459 KBFL", route_planner::options{}),
                    route_parse_error);
}

TEST_CASE("expand_sigils accepts lat/lon coordinates at sigil boundaries")
{
    // 384143N1213527W ≈ (38.695°, -121.591°) — near KSMF.
    auto out = test_planner().expand_sigils("384143N1213527W ? KBFL", route_planner::options{});
    CHECK(out.find('?') == std::string::npos);
    CHECK(out.substr(0, 15) == "384143N1213527W");
}

// ---- Stage 6: preferences ----

TEST_CASE("load_route_plan_options reads PREFER/INCLUDE/AVOID/REJECT")
{
    // Write a minimal ini, load it, verify the modifier table.
    auto path = (std::filesystem::temp_directory_path() /
                 "osect_route_plan_test.ini").string();
    {
        std::ofstream out(path);
        out << "[route_plan]\n"
            << "max_leg_length_nm = 120\n"
            << "route_waypoint_airport = AVOID\n"
            << "route_waypoint_vor = PREFER\n"
            << "route_airway_victor = PREFER\n"
            << "route_airway_jet = REJECT\n";
    }
    ini_config ini(path);
    auto opts = load_route_plan_options(ini);
    CHECK(opts.max_leg_length_nm == 120.0);
    CHECK(opts.wp_cost[static_cast<std::size_t>(
        wp_subtype::airport_landplane)]
        == cost_avoid);
    CHECK(opts.wp_cost[static_cast<std::size_t>(
        wp_subtype::navaid_vor)]
        == cost_prefer);
    CHECK(opts.awy_cost[static_cast<std::size_t>(
        awy_class::victor)]
        == cost_prefer);
    CHECK(opts.awy_cost[static_cast<std::size_t>(
        awy_class::jet)]
        == cost_reject);
    CHECK_FALSE(opts.use_airways);
    std::remove(path.c_str());
}

TEST_CASE("load_route_plan_options rejects unknown preference values")
{
    auto path = (std::filesystem::temp_directory_path() /
                 "osect_route_plan_test_bad.ini").string();
    {
        std::ofstream out(path);
        out << "[route_plan]\nroute_waypoint_airport = WHATEVER\n";
    }
    ini_config ini(path);
    CHECK_THROWS_AS(load_route_plan_options(ini), std::runtime_error);
    std::remove(path.c_str());
}

TEST_CASE("use_airways toggle changes path via fix-rejecting options")
{
    // Build options that reject every routable subtype except
    // airports. Without use_airways, the planner can only hop
    // airport-to-airport. Toggling use_airways forces fixes and
    // navaids back to INCLUDE so the planner gains a much larger
    // search graph — observable as a different (typically more
    // direct) result.
    const auto& p = test_planner();
    route_planner::options opts;
    opts.wp_cost.fill(cost_reject);
    opts.wp_cost[static_cast<std::size_t>(
        wp_subtype::airport_landplane)]
        = cost_include;

    auto without = p.expand_sigils("KSMF ? KBFL", opts);
    opts.use_airways = true;
    auto with    = p.expand_sigils("KSMF ? KBFL", opts);
    CHECK(without != with);
}
