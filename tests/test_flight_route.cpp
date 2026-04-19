#include "../src/flight_route.hpp"
#include <cassert>
#include <cstdio>

using namespace nasrbrowse;

static void test_latlon_waypoint(nasr_database& db)
{
    flight_route route("383412N1210305W 383412N1200000W", db);
    assert(route.waypoints.size() == 2);
    assert(std::holds_alternative<latlon_ref>(route.waypoints[0]));

    auto& ll = std::get<latlon_ref>(route.waypoints[0]);
    assert(std::abs(ll.lat - 38.57) < 0.01);
    assert(std::abs(ll.lon - (-121.0514)) < 0.01);

    // Round-trip: waypoint_id should reproduce the DDMMSSXDDDMMSSY format
    auto id = waypoint_id(route.waypoints[0]);
    assert(id == "383412N1210305W");

    std::printf("  latlon_waypoint: OK\n");
}

static void test_simple_route(nasr_database& db)
{
    flight_route route("O61 KMER", db);
    assert(route.waypoints.size() == 2);
    assert(waypoint_id(route.waypoints[0]) == "O61");
    assert(waypoint_id(route.waypoints[1]) == "KMER");
    std::printf("  simple_route: OK (%zu waypoints)\n", route.waypoints.size());
}

static void test_route_with_navaid(nasr_database& db)
{
    flight_route route("O61 LIN KMER", db);
    assert(route.waypoints.size() == 3);
    assert(std::holds_alternative<navaid_ref>(route.waypoints[1]));
    std::printf("  route_with_navaid: OK\n");
}

static void test_route_with_airway(nasr_database& db)
{
    flight_route route("O61 LIN V459 LOPES KTSP", db);
    std::printf("  route_with_airway: OK (%zu waypoints, %zu elements)\n",
                route.waypoints.size(), route.elements.size());
    std::printf("    waypoints:");
    for(const auto& wp : route.waypoints)
        std::printf(" %s", waypoint_id(wp).c_str());
    std::printf("\n");
    std::printf("    text: %s\n", route.to_text().c_str());
}

static void test_route_with_latlon(nasr_database& db)
{
    flight_route route("383412N1210305W LIN", db);
    assert(route.waypoints.size() == 2);
    assert(std::holds_alternative<latlon_ref>(route.waypoints[0]));
    std::printf("  route_with_latlon: OK\n");
}

static void test_unknown_waypoint(nasr_database& db)
{
    try
    {
        flight_route route("ZZZZZ KMER", db);
        assert(false && "should have thrown");
    }
    catch(const route_parse_error& e)
    {
        assert(std::string(e.what()).find("unknown") != std::string::npos);
        assert(e.token == "ZZZZZ");
        assert(e.token_index == 0);
        std::printf("  unknown_waypoint: OK (error: %s)\n", e.what());
    }
}

static void test_to_text(nasr_database& db)
{
    flight_route route("O61 LIN V459 LOPES KTSP", db);
    auto text = route.to_text();
    std::printf("  to_text: %s\n", text.c_str());
    assert(text.find("V459") != std::string::npos);
    assert(text.find("LOPES") != std::string::npos);
}

static void test_insert_waypoint(nasr_database& db)
{
    flight_route route("O61 KMER", db);
    assert(route.waypoints.size() == 2);

    auto navs = db.lookup_navaids("LIN");
    assert(!navs.empty());
    route.insert_waypoint(0, navaid_ref{"LIN", std::move(navs.front())});

    assert(route.waypoints.size() == 3);
    assert(waypoint_id(route.waypoints[0]) == "O61");
    assert(waypoint_id(route.waypoints[1]) == "LIN");
    assert(waypoint_id(route.waypoints[2]) == "KMER");
    std::printf("  insert_waypoint: OK\n");
}

static void test_single_waypoint(nasr_database& db)
{
    try
    {
        flight_route route("O61", db);
        assert(false && "should have thrown");
    }
    catch(const route_parse_error& e)
    {
        assert(std::string(e.what()).find("at least two") != std::string::npos);
        assert(e.token_index == -1);
        std::printf("  single_waypoint: OK (error: %s)\n", e.what());
    }
}

int main()
{
    std::printf("flight_route tests:\n");
    nasr_database db("nasr.db");
    test_latlon_waypoint(db);
    test_simple_route(db);
    test_route_with_navaid(db);
    test_route_with_airway(db);
    test_route_with_latlon(db);
    test_unknown_waypoint(db);
    test_to_text(db);
    test_insert_waypoint(db);
    test_single_waypoint(db);

    std::printf("\nAll tests passed.\n");
    return 0;
}
