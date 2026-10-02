#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "flight_route.hpp"  // route_parse_error
#include "ini_config.hpp"
#include "nasr_database.hpp"
#include "route_plan_config.hpp"
#include "elevation_source.hpp"
#include "route_planner.hpp"
#include "tmp_user_db.hpp"
#include "user_database.hpp"
#include "geo_math.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>

using namespace osect;

// Shared DB/planner for all tests. Built once on first use; the test
// binary is expected to run with the repo root as its working directory.
static const nasr_database& test_db()
{
    static const nasr_database db("osect.db");
    return db;
}

static const elevation_source& no_terrain()
{
    static const elevation_source terrain("missing-terrain-tree");
    return terrain;
}

static const route_planner& test_planner()
{
    static test::tmp_user_db user_db("planner_shared");
    static route_planner planner("osect.db", user_db.db_file, no_terrain());
    return planner;
}

// A planner of its own for the cancellation tests, so a request they
// leave set cannot affect the shared planner.
static route_planner& cancellable_planner()
{
    static test::tmp_user_db user_db("planner_cancel");
    static route_planner planner("osect.db", user_db.db_file, no_terrain());
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

// ---- A* core ----

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

// Cost modifier of node `idx` under `opts` (airways off), 1.0 for the
// segment's origin and destination.
static double node_modifier(const route_planner& p, std::size_t idx, std::size_t origin, std::size_t destination,
                            const route_planner::options& opts)
{
    if(idx == origin || idx == destination)
    {
        return 1.0;
    }
    return opts.wp_cost.at(static_cast<std::size_t>(rpta::get_node(p, idx).subtype));
}

// Cost of the leg a → b: its length times both ends' modifiers.
static double leg_cost(const route_planner& p, std::size_t a, std::size_t b, std::size_t origin,
                       std::size_t destination, const route_planner::options& opts)
{
    const auto& na = rpta::get_node(p, a);
    const auto& nb = rpta::get_node(p, b);
    return haversine_distance_nm(na.lat, na.lon, nb.lat, nb.lon) *
           node_modifier(p, a, origin, destination, opts) * node_modifier(p, b, origin, destination, opts);
}

// Cost of origin → path → destination.
static double path_cost(const route_planner& p, std::size_t origin, const std::vector<std::size_t>& path,
                        std::size_t destination, const route_planner::options& opts)
{
    std::vector<std::size_t> all{origin};
    all.insert(all.end(), path.begin(), path.end());
    all.push_back(destination);
    double total = 0.0;
    for(std::size_t i = 0; i + 1 < all.size(); ++i)
    {
        total += leg_cost(p, all[i], all[i + 1], origin, destination, opts);
    }
    return total;
}

// Cheapest origin → destination cost over legs of at most max_leg
// between nodes inside the endpoints' bounding box grown by
// `margin_deg`, by Dijkstra over the complete graph.
static double cheapest_in_box(const route_planner& p, std::size_t origin, std::size_t destination,
                              const route_planner::options& opts, double margin_deg)
{
    const auto& o = rpta::get_node(p, origin);
    const auto& d = rpta::get_node(p, destination);
    const double lat_min = std::min(o.lat, d.lat) - margin_deg;
    const double lat_max = std::max(o.lat, d.lat) + margin_deg;
    const double lon_min = std::min(o.lon, d.lon) - margin_deg;
    const double lon_max = std::max(o.lon, d.lon) + margin_deg;
    std::vector<std::size_t> box;
    for(std::size_t i = 0; i < rpta::node_count(p); ++i)
    {
        const auto& n = rpta::get_node(p, i);
        if(n.lat >= lat_min && n.lat <= lat_max && n.lon >= lon_min && n.lon <= lon_max)
        {
            box.push_back(i);
        }
    }
    const auto inf = std::numeric_limits<double>::infinity();
    std::vector<double> cost(box.size(), inf);
    std::vector<bool> done(box.size(), false);
    const auto start = static_cast<std::size_t>(std::find(box.begin(), box.end(), origin) - box.begin());
    const auto goal = static_cast<std::size_t>(std::find(box.begin(), box.end(), destination) - box.begin());
    cost[start] = 0.0;
    for(;;)
    {
        std::size_t u = box.size();
        for(std::size_t i = 0; i < box.size(); ++i)
        {
            if(!done[i] && cost[i] < inf && (u == box.size() || cost[i] < cost[u]))
            {
                u = i;
            }
        }
        if(u == box.size() || u == goal)
        {
            return cost[goal];
        }
        done[u] = true;
        const auto& a = rpta::get_node(p, box[u]);
        for(std::size_t v = 0; v < box.size(); ++v)
        {
            const auto& b = rpta::get_node(p, box[v]);
            if(done[v] || haversine_distance_nm(a.lat, a.lon, b.lat, b.lon) > opts.max_leg_length_nm)
            {
                continue;
            }
            cost[v] = std::min(cost[v], cost[u] + leg_cost(p, box[u], box[v], origin, destination, opts));
        }
    }
}

TEST_CASE("plan_segment costs the final leg")
{
    // Every modifier is at least 1, so the direct-leg shortcut holds and
    // the planner's route is the cheapest; it can't cost more than the
    // cheapest one restricted to a box. With AVOID nodes, the final leg
    // out of one costs extra and must count toward the route's cost.
    const auto& p = test_planner();
    route_planner::options opts;
    opts.wp_cost.fill(cost_avoid);
    opts.wp_cost.at(static_cast<std::size_t>(wp_subtype::airport_landplane)) = cost_include;
    opts.wp_cost.at(static_cast<std::size_t>(wp_subtype::navaid_vor)) = cost_include;
    for(const auto& [from, to] : {std::pair{"KLAX", "KSAN"}, std::pair{"KSFO", "KBIH"}})
    {
        CAPTURE(from);
        CAPTURE(to);
        const auto origin = *rpta::node_index(p, from);
        const auto destination = *rpta::node_index(p, to);
        const auto path = rpta::plan_segment(p, endpoint_at(p, origin), endpoint_at(p, destination), opts);
        REQUIRE(path.has_value());
        // The two sums add in different orders.
        CHECK(path_cost(p, origin, *path, destination, opts) <=
              cheapest_in_box(p, origin, destination, opts, 1.0) + 1e-9);
    }
}

TEST_CASE("plan_segment throws route_plan_cancelled after request_cancel")
{
    // KSMF to KBFL needs A* steps (see above), so the request is seen.
    auto& p = cancellable_planner();
    auto ksmf = *rpta::node_index(p, "KSMF");
    auto kbfl = *rpta::node_index(p, "KBFL");
    p.request_cancel();
    CHECK_THROWS_AS(rpta::plan_segment(p, endpoint_at(p, ksmf), endpoint_at(p, kbfl), {}), route_plan_cancelled);
    p.clear_cancel();
}

TEST_CASE("plan_segment plans normally after clear_cancel")
{
    auto& p = cancellable_planner();
    auto ksmf = *rpta::node_index(p, "KSMF");
    auto kbfl = *rpta::node_index(p, "KBFL");
    p.request_cancel();
    p.clear_cancel();
    auto path = rpta::plan_segment(p, endpoint_at(p, ksmf), endpoint_at(p, kbfl), {});
    REQUIRE(path.has_value());
    CHECK_FALSE(path->empty());
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

// ---- expand_sigils grammar ----

TEST_CASE("expand_sigils is a no-op when input has no '?'")
{
    CHECK(test_planner().expand_sigils("KSMF V23 KBFL", route_planner::options{}).text == "KSMF V23 KBFL");
}

TEST_CASE("expand_sigils rewrites '?' with intermediate IDs")
{
    auto out = test_planner().expand_sigils("KSMF ? KBFL", route_planner::options{}).text;
    CHECK(out.find('?') == std::string::npos);
    CHECK(out.substr(0, 4) == "KSMF");
    CHECK(out.substr(out.size() - 4) == "KBFL");
    // ~230 nm between the airports means the planner must have
    // inserted at least one intermediate.
    CHECK(out.size() > std::string("KSMF KBFL").size());
}

TEST_CASE("expand_sigils accepts '?' without surrounding whitespace")
{
    auto a = test_planner().expand_sigils("KSMF?KBFL", route_planner::options{}).text;
    auto b = test_planner().expand_sigils("KSMF ? KBFL", route_planner::options{}).text;
    CHECK(a == b);
}

TEST_CASE("expand_sigils throws route_plan_cancelled after request_cancel")
{
    // KSMF-KSAC is a direct leg with no A* steps; the request is
    // checked before each sigil segment.
    auto& p = cancellable_planner();
    p.request_cancel();
    CHECK_THROWS_AS(p.expand_sigils("KSMF ? KSAC", route_planner::options{}).text, route_plan_cancelled);
    p.clear_cancel();
}

TEST_CASE("expand_sigils rejects leading '?'")
{
    CHECK_THROWS_AS(test_planner().expand_sigils("? KBFL", route_planner::options{}).text, route_parse_error);
}

TEST_CASE("expand_sigils rejects trailing '?'")
{
    CHECK_THROWS_AS(test_planner().expand_sigils("KSMF ?", route_planner::options{}).text, route_parse_error);
}

TEST_CASE("expand_sigils rejects consecutive '?'")
{
    CHECK_THROWS_AS(test_planner().expand_sigils("KSMF ? ? KBFL", route_planner::options{}).text,
                    route_parse_error);
}

// ---- airway-adjacent sigil (project-and-walk) ----

TEST_CASE("expand_sigils: '?' on both sides of an airway")
{
    // KSMF ? V23 ? KBFL: entry and exit on V23 via project-and-walk,
    // A* on both sides. The expanded text should contain V23 and
    // should re-parse successfully as a flight_route.
    auto out = test_planner().expand_sigils("KSMF ? V23 ? KBFL", route_planner::options{}).text;
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
    auto out = test_planner().expand_sigils("KSMF V23 ? KBFL", route_planner::options{}).text;
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
    auto out = test_planner().expand_sigils("KSMF ? V23 KBFL", route_planner::options{}).text;
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
    CHECK_THROWS_AS(test_planner().expand_sigils("KSMF V23 ? V459 KBFL", route_planner::options{}).text,
                    route_parse_error);
}

TEST_CASE("expand_sigils accepts lat/lon coordinates at sigil boundaries")
{
    // 384143N1213527W ≈ (38.695°, -121.591°) — near KSMF.
    auto out = test_planner().expand_sigils("384143N1213527W ? KBFL", route_planner::options{}).text;
    CHECK(out.find('?') == std::string::npos);
    CHECK(out.substr(0, 15) == "384143N1213527W");
}

// ---- preferences ----

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

// Loads route_plan options from an ini holding `body` under [route_plan].
static route_planner::options load_route_plan_body(const std::string& body)
{
    auto path = (std::filesystem::temp_directory_path() / "osect_route_plan_test_avoid.ini").string();
    {
        std::ofstream out(path);
        out << "[route_plan]\n" << body;
    }
    struct remover
    {
        std::string path;
        ~remover()
        {
            std::remove(path.c_str());
        }
    } remove{path};
    return load_route_plan_options(ini_config(path));
}

TEST_CASE("load_route_plan_options defaults avoid_terrain to false")
{
    CHECK_FALSE(load_route_plan_options(ini_config{}).avoid_terrain);
}

TEST_CASE("load_route_plan_options reads avoid_terrain true and false")
{
    CHECK(load_route_plan_body("avoid_terrain = true\n").avoid_terrain);
    CHECK(load_route_plan_body("avoid_terrain = TRUE\n").avoid_terrain);
    CHECK_FALSE(load_route_plan_body("avoid_terrain = false\n").avoid_terrain);
}

TEST_CASE("load_route_plan_options rejects other avoid_terrain values")
{
    CHECK_THROWS_AS(load_route_plan_body("avoid_terrain = yes\n"), std::runtime_error);
    CHECK_THROWS_AS(load_route_plan_body("avoid_terrain = REJECT\n"), std::runtime_error);
}

TEST_CASE("validate_route_plan_options accepts default options")
{
    const route_planner::options options;
    CHECK(validate_route_plan_options(options).empty());
}

TEST_CASE("validate_route_plan_options rejects non-positive max_leg_length_nm")
{
    route_planner::options o;
    o.max_leg_length_nm = 0.0;
    CHECK_FALSE(validate_route_plan_options(o).empty());
    o.max_leg_length_nm = -1.0;
    CHECK_FALSE(validate_route_plan_options(o).empty());
}

TEST_CASE("load_route_plan_options rejects non-positive max_leg_length_nm")
{
    auto path = (std::filesystem::temp_directory_path() /
                 "osect_route_plan_test_max_leg.ini").string();
    {
        std::ofstream out(path);
        out << "[route_plan]\nmax_leg_length_nm = 0\n";
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

    auto without = p.expand_sigils("KSMF ? KBFL", opts).text;
    opts.use_airways = true;
    auto with    = p.expand_sigils("KSMF ? KBFL", opts).text;
    CHECK(without != with);
}

// ---- user waypoints ----

namespace
{
    // load_route_plan_options + force a single subtype to the given
    // cost. The defaults from osect.ini set route_waypoint_user =
    // PREFER, but tests are isolated from osect.ini, so we set the
    // value explicitly.
    route_planner::options user_opts(double user_cost)
    {
        route_planner::options o;
        o.wp_cost[static_cast<std::size_t>(wp_subtype::user)] = user_cost;
        return o;
    }
}

TEST_CASE("user waypoint resolves by name through node_index after refresh")
{
    test::tmp_user_db tmp("resolve");
    {
        user_database udb(tmp.db_file);
        auto w = udb.insert_waypoint(38.1, -121.0);
        CHECK(w.name == "WPT1");
    }
    route_planner p("osect.db", tmp.db_file, no_terrain());
    // The refresh that exposes user waypoints fires inside
    // expand_sigils; node_index doesn't refresh on its own.
    p.expand_sigils("KSMF KSAC", route_planner::options{}).text;
    auto idx = rpta::node_index(p, "WPT1");
    REQUIRE(idx.has_value());
    const auto& n = rpta::get_node(p, *idx);
    CHECK(n.kind == rpta::node_kind::user);
    CHECK(n.lat == 38.1);
    CHECK(n.lon == -121.0);
}

TEST_CASE("expand_sigils routes through a preferred user waypoint")
{
    // KSMF (38.70, -121.59) → KMER (37.28, -120.51) is ~110 nm, so
    // the default 80 nm max_leg short-circuit doesn't fire and A*
    // has to find an intermediate. WPT1 at the midpoint sits ~50 nm
    // from each airport. Origin and destination are passed to A* as
    // synthetic endpoints, so their wp_cost isn't consulted — every
    // NASR subtype can be REJECT'd to leave WPT1 the only viable
    // intermediate.
    test::tmp_user_db tmp("prefer");
    {
        user_database udb(tmp.db_file);
        udb.insert_waypoint(37.99, -121.05);
    }
    route_planner p("osect.db", tmp.db_file, no_terrain());

    auto opts = user_opts(cost_prefer);
    for(std::size_t i = 0; i < opts.wp_cost.size(); ++i)
    {
        if(static_cast<wp_subtype>(i) != wp_subtype::user)
        {
            opts.wp_cost[i] = cost_reject;
        }
    }

    auto out = p.expand_sigils("KSMF ? KMER", opts).text;
    CHECK(out.find("WPT1") != std::string::npos);
}

TEST_CASE("expand_sigils ignores a rejected user waypoint")
{
    // Same midway waypoint, but with user=REJECT. The planner must
    // pick some other intermediate.
    test::tmp_user_db tmp("reject");
    {
        user_database udb(tmp.db_file);
        udb.insert_waypoint(37.99, -121.05);
    }
    route_planner p("osect.db", tmp.db_file, no_terrain());

    auto opts = user_opts(cost_reject);
    auto out = p.expand_sigils("KSMF ? KMER", opts).text;
    CHECK(out.find("WPT1") == std::string::npos);
}

TEST_CASE("user waypoint set refreshes between expand_sigils calls")
{
    test::tmp_user_db tmp("refresh");
    route_planner p("osect.db", tmp.db_file, no_terrain());

    // No waypoints yet — WPT1 must not resolve.
    p.expand_sigils("KSMF KSAC", route_planner::options{}).text;
    CHECK_FALSE(rpta::node_index(p, "WPT1").has_value());

    // Insert a waypoint and re-run expand_sigils to trigger refresh.
    {
        user_database udb(tmp.db_file);
        udb.insert_waypoint(38.1, -121.0);
    }
    p.expand_sigils("KSMF KSAC", route_planner::options{}).text;
    CHECK(rpta::node_index(p, "WPT1").has_value());

    // Delete it and refresh again — the node disappears from the catalog.
    {
        user_database udb(tmp.db_file);
        auto wps = udb.load_waypoints();
        REQUIRE(wps.size() == 1);
        udb.delete_waypoint(wps[0].waypoint_id);
    }
    p.expand_sigils("KSMF KSAC", route_planner::options{}).text;
    CHECK_FALSE(rpta::node_index(p, "WPT1").has_value());
}

TEST_CASE("load_route_plan_options loads route_waypoint_user")
{
    auto path = (std::filesystem::temp_directory_path() /
                 "osect_route_plan_test_user.ini").string();
    {
        std::ofstream out(path);
        out << "[route_plan]\nroute_waypoint_user = AVOID\n";
    }
    ini_config ini(path);
    auto opts = load_route_plan_options(ini);
    CHECK(opts.wp_cost[static_cast<std::size_t>(wp_subtype::user)] == cost_avoid);
    std::remove(path.c_str());
}

TEST_CASE("load_route_plan_options defaults route_waypoint_user to PREFER")
{
    auto path = (std::filesystem::temp_directory_path() /
                 "osect_route_plan_test_user_default.ini").string();
    {
        std::ofstream out(path);
        out << "[route_plan]\n";
    }
    ini_config ini(path);
    auto opts = load_route_plan_options(ini);
    CHECK(opts.wp_cost[static_cast<std::size_t>(wp_subtype::user)] == cost_prefer);
    std::remove(path.c_str());
}
