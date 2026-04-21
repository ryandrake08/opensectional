#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "flight_route.hpp"

#include <cmath>
#include <string>

using namespace nasrbrowse;

// Shared DB for all tests. Opened lazily on first use; the test binary is
// expected to run with the repo root as its working directory (see
// tests/CMakeLists.txt — WORKING_DIRECTORY is set to the source tree).
static nasr_database& test_db()
{
    static nasr_database db("nasr.db");
    return db;
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

    // Airway expands into intermediate fixes, so waypoint count > element count
    CHECK(route.waypoints.size() > route.elements.size());

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
    route.insert_waypoint(0, navaid_ref{"LIN", std::move(navs.front())});

    REQUIRE(route.waypoints.size() == 3);
    CHECK(waypoint_id(route.waypoints[0]) == "O61");
    CHECK(waypoint_id(route.waypoints[1]) == "LIN");
    CHECK(waypoint_id(route.waypoints[2]) == "KMER");
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
