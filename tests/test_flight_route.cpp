#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "flight_route.hpp"
#include "route_planner.hpp"

#include <cmath>
#include <string>

using namespace osect;

// Shared DB for all tests. Opened lazily on first use; the test binary is
// expected to run with the repo root as its working directory (see
// tests/CMakeLists.txt — WORKING_DIRECTORY is set to the source tree).
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

TEST_CASE("latlon waypoint parses DDMMSSXDDDMMSSY and round-trips")
{
    flight_route route("383412N1210305W 383412N1200000W", test_db());
    REQUIRE(route.waypoints.size() == 2);
    REQUIRE(std::holds_alternative<latlon_ref>(route.waypoints[0]));

    const auto& ll = std::get<latlon_ref>(route.waypoints[0]);
    CHECK(std::abs(ll.lat - 38.57) < 0.01);
    CHECK(std::abs(ll.lon - (-121.0514)) < 0.01);

    CHECK(waypoint_id(route.waypoints[0]) == "383412N1210305W");
}

TEST_CASE("simple airport-to-airport route")
{
    flight_route route("O61 KMER", test_db());
    REQUIRE(route.waypoints.size() == 2);
    CHECK(waypoint_id(route.waypoints[0]) == "O61");
    CHECK(waypoint_id(route.waypoints[1]) == "KMER");
}

TEST_CASE("route with navaid in the middle")
{
    flight_route route("O61 LIN KMER", test_db());
    REQUIRE(route.waypoints.size() == 3);
    CHECK(std::holds_alternative<navaid_ref>(route.waypoints[1]));
}

TEST_CASE("route with airway expansion")
{
    flight_route route("O61 LIN V459 LOPES KTSP", test_db());

    // Airway expands into intermediate fixes, so waypoint count
    // exceeds the four typed tokens (O61, LIN, LOPES, KTSP).
    CHECK(route.waypoints.size() > 4);

    // to_text round-trips the airway shorthand
    const std::string text = route.to_text();
    CHECK(text.find("V459") != std::string::npos);
    CHECK(text.find("LOPES") != std::string::npos);
}

TEST_CASE("route beginning with lat/lon and ending at navaid")
{
    flight_route route("383412N1210305W LIN", test_db());
    REQUIRE(route.waypoints.size() == 2);
    CHECK(std::holds_alternative<latlon_ref>(route.waypoints[0]));
}

TEST_CASE("bare FAA ID of an ICAO-assigned airport resolves to a navaid")
{
    // "SAC" is both the FAA ID of KSAC (Sacramento Executive) and the
    // ID of the SACRAMENTO VORTAC. User-entered routes should prefer
    // the navaid; the ICAO ID "KSAC" is required to pick the airport.
    flight_route route("SAC KSAC", test_db());
    REQUIRE(route.waypoints.size() == 2);
    CHECK(std::holds_alternative<navaid_ref>(route.waypoints[0]));
    CHECK(std::holds_alternative<airport_ref>(route.waypoints[1]));
}

TEST_CASE("unknown waypoint throws route_parse_error")
{
    try
    {
        flight_route route("ZZZZZ KMER", test_db());
        FAIL("expected route_parse_error");
    }
    catch(const route_parse_error& e)
    {
        CHECK(std::string(e.what()).find("unknown") != std::string::npos);
        CHECK(e.token == "ZZZZZ");
        CHECK(e.token_index == 0);
    }
}

TEST_CASE("insert_waypoint adds a waypoint at the given segment")
{
    flight_route route("O61 KMER", test_db());
    REQUIRE(route.waypoints.size() == 2);

    auto navs = test_db().lookup_navaids("LIN");
    REQUIRE_FALSE(navs.empty());
    route.insert_waypoint(0, navaid_ref{"LIN", std::move(navs.front())},
                          test_db());

    REQUIRE(route.waypoints.size() == 3);
    CHECK(waypoint_id(route.waypoints[0]) == "O61");
    CHECK(waypoint_id(route.waypoints[1]) == "LIN");
    CHECK(waypoint_id(route.waypoints[2]) == "KMER");
}

TEST_CASE("airway_ize collapses consecutive waypoints on a common airway")
{
    // V459 begins SLI -> DODGR -> DARTS -> BERRI -> KIMMO -> SAUGS.
    // The five typed fixes collapse to "SLI V459 KIMMO" shorthand,
    // but the expanded waypoint list is unchanged.
    flight_route route("SLI DODGR DARTS BERRI KIMMO", test_db());
    REQUIRE(route.waypoints.size() == 5);
    CHECK(route.to_text() == "SLI V459 KIMMO");
}

TEST_CASE("airway_ize leaves 2-waypoint routes alone")
{
    // SLI -> DODGR are adjacent on V459 but a single-hop run doesn't
    // save anything when collapsed to "SLI V459 DODGR".
    flight_route route("SLI DODGR", test_db());
    REQUIRE(route.waypoints.size() == 2);
    CHECK(route.to_text() == "SLI DODGR");
}

TEST_CASE("airway_ize does not collapse fixes skipping a bent airway's intermediates")
{
    // A337 goes JUNIE -> AXIDE -> FONUG -> SNAPP -> TEEDE -> TEGOD
    // with large bends: AXIDE is already ~4.5 NM off the direct
    // JUNIE->TEGOD great circle, far outside the 0.5 NM tolerance.
    // The user's "direct JUNIE TEGOD" must be preserved.
    flight_route route("JUNIE TEGOD", test_db());
    REQUIRE(route.waypoints.size() == 2);
    CHECK(route.to_text() == "JUNIE TEGOD");
}

TEST_CASE("coerce fills colinear intermediates on a visually straight airway")
{
    // V459 is a visually-straight run: SLI -> DODGR -> DARTS -> BERRI
    // -> KIMMO. The user typed SLI KIMMO direct, but every intermediate
    // has XTE < 0.001 NM from the direct great circle, so coerce
    // inserts them and the collapse pass folds the run into shorthand.
    flight_route route("SLI KIMMO", test_db());
    CHECK(route.to_text() == "SLI V459 KIMMO");
    // Coerce inserted DODGR, DARTS, BERRI between SLI and KIMMO.
    CHECK(route.waypoints.size() == 5);
}

TEST_CASE("coerce does not fire when endpoints share no airway")
{
    // O61 is an airport (not on any airway) and KMER is also an
    // airport. Direct leg must stay direct.
    flight_route route("O61 KMER", test_db());
    CHECK(route.to_text() == "O61 KMER");
    CHECK(route.waypoints.size() == 2);
}

TEST_CASE("coerce is all-or-nothing: one bent intermediate vetoes the whole run")
{
    // JUNIE and TEGOD are the endpoints of A337. Even though they share
    // an airway, AXIDE alone is ~4.5 NM off the direct line, which
    // exceeds the 0.5 NM tolerance on the very first check. Nothing
    // gets inserted — not even the failing AXIDE.
    flight_route route("JUNIE TEGOD", test_db());
    REQUIRE(route.waypoints.size() == 2);
    CHECK(waypoint_id(route.waypoints[0]) == "JUNIE");
    CHECK(waypoint_id(route.waypoints[1]) == "TEGOD");
}

TEST_CASE("coerce does not affect pairs that are already adjacent on an airway")
{
    // SLI and DODGR are adjacent on V459 (no intermediates to insert).
    // Coerce has nothing to do; collapse also leaves it alone because
    // length-2 runs aren't worth collapsing.
    flight_route route("SLI DODGR", test_db());
    CHECK(route.to_text() == "SLI DODGR");
}

TEST_CASE("expand_airway splits at published discontinuities")
{
    // V23 has a gap_flag='Y' segment between FRAME and EBTUW, so a
    // traversal that crosses it must bridge with explicit waypoints.
    // KSMF auto-corrects the entry to CAPTO (high seq) and KBFL
    // auto-corrects the exit to EHF (low seq), so the walk passes
    // right through the gap. The shorthand has two V23 spans
    // joined by FRAME — one before the gap, one after.
    flight_route route("KSMF V23 KBFL", test_db());
    CHECK(route.to_text() == "KSMF CAPTO V23 EBTUW FRAME V23 EHF KBFL");
}

TEST_CASE("coerce does not connect fixes across a published airway gap")
{
    // EBTUW and FRAME are both on V23 but only as endpoints of its
    // gap segment. A user-typed direct "EBTUW FRAME" must stay direct.
    flight_route route("EBTUW FRAME", test_db());
    CHECK(route.to_text() == "EBTUW FRAME");
    REQUIRE(route.waypoints.size() == 2);
}

TEST_CASE("coerce's iterative anchor handles long airways a single-pass check would reject")
{
    // AR16 goes PERMT -> LENDS -> GRUBR -> SNABS -> SEELO over 279 NM.
    // The mid fix's XTE from the direct PERMT->SEELO great circle is
    // 0.82 NM (would fail a 0.5 NM single-pass threshold), but each
    // hop's XTE from the current anchor is <0.5 NM, so the iterative
    // coerce succeeds and collapses into AR16 shorthand.
    flight_route route("PERMT SEELO", test_db());
    CHECK(route.to_text() == "PERMT AR16 SEELO");
}

TEST_CASE("airway_ize re-collapses after delete_waypoint flattens an airway")
{
    // Initial route uses V459 shorthand which parses into 6 waypoints.
    flight_route route("SLI V459 SAUGS", test_db());
    REQUIRE(route.waypoints.size() == 6);

    // Delete SAUGS (the last waypoint). The remaining 5 waypoints are
    // still sequential on V459, so airway_ize re-collapses to "SLI
    // V459 KIMMO".
    auto last = route.waypoints.size() - 1;
    route.delete_waypoint(last, test_db());

    REQUIRE(route.waypoints.size() == 5);
    CHECK(route.to_text() == "SLI V459 KIMMO");
}

TEST_CASE("single waypoint route is rejected")
{
    try
    {
        flight_route route("O61", test_db());
        FAIL("expected route_parse_error");
    }
    catch(const route_parse_error& e)
    {
        CHECK(std::string(e.what()).find("at least two") != std::string::npos);
        CHECK(e.token_index == -1);
    }
}

// ---- sigil pipeline ----

TEST_CASE("flight_route rejects raw '?' tokens")
{
    // '?' is a preprocessor directive, not part of flight_route's
    // grammar. Callers must run the text through
    // route_planner::expand_sigils first.
    CHECK_THROWS_AS(flight_route("KSMF ? KBFL", test_db()),
                    route_parse_error);
}

TEST_CASE("expand_sigils + flight_route plans between two airports")
{
    auto expanded = test_planner().expand_sigils("KSMF ? KBFL", route_planner::options{});
    flight_route route(expanded, test_db());
    REQUIRE(route.waypoints.size() > 2);
    CHECK(waypoint_id(route.waypoints.front()) == "KSMF");
    CHECK(waypoint_id(route.waypoints.back()) == "KBFL");
}

TEST_CASE("expand_sigils preserves intermediate via waypoints")
{
    // KSMF ? LIN ? KBFL plans KSMF→LIN, then LIN→KBFL. LIN must
    // survive as a waypoint.
    auto expanded = test_planner().expand_sigils("KSMF ? LIN ? KBFL", route_planner::options{});
    flight_route route(expanded, test_db());
    CHECK(waypoint_id(route.waypoints.front()) == "KSMF");
    CHECK(waypoint_id(route.waypoints.back()) == "KBFL");
    bool has_lin = false;
    for(const auto& wp : route.waypoints)
        if(waypoint_id(wp) == "LIN") { has_lin = true; break; }
    CHECK(has_lin);
}

TEST_CASE("expand_sigils leaves direct segments alone")
{
    // KSMF ? LIN KBFL plans only the first leg; LIN→KBFL is direct.
    auto expanded = test_planner().expand_sigils("KSMF ? LIN KBFL", route_planner::options{});
    flight_route route(expanded, test_db());
    CHECK(waypoint_id(route.waypoints.front()) == "KSMF");
    CHECK(waypoint_id(route.waypoints.back()) == "KBFL");
}

TEST_CASE("planned route round-trips through to_text")
{
    auto expanded = test_planner().expand_sigils("KSMF ? KBFL", route_planner::options{});
    flight_route route(expanded, test_db());
    auto text = route.to_text();
    CHECK(text.find('?') == std::string::npos);
    flight_route reparsed(text, test_db());
    REQUIRE(reparsed.waypoints.size() == route.waypoints.size());
    for(std::size_t i = 0; i < route.waypoints.size(); ++i)
        CHECK(waypoint_id(reparsed.waypoints[i])
              == waypoint_id(route.waypoints[i]));
}
