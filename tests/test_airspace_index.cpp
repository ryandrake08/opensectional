#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "airspace_index.hpp"
#include "altitude_filter.hpp"
#include "elevation_address.hpp"
#include "elevation_source.hpp"
#include "elevation_test_tree.hpp"
#include "ephemeral_database.hpp"
#include "nasr_database.hpp"

#include <string>

using namespace osect;

namespace
{
    // A lon/lat-aligned square ring.
    airspace_ring square(double lon0, double lat0, double lon1, double lat1, bool is_hole = false)
    {
        return {{{lat0, lon0}, {lat0, lon1}, {lat1, lon1}, {lat1, lon0}}, is_hole};
    }

    // A restricted area over 36..37 N, 100..99 W from 2,000 to 10,000 ft MSL.
    airspace_volume restricted_block()
    {
        return {airspace_class::restricted, "R-TEST", 2000.0, 10000.0, {square(-100.0, 36.0, -99.0, 37.0)}};
    }

    airspace_costs rejecting_restricted()
    {
        airspace_costs costs;
        costs.fill(cost_include);
        costs.at(static_cast<std::size_t>(airspace_class::restricted)) = cost_reject;
        costs.at(static_cast<std::size_t>(airspace_class::warning)) = cost_avoid;
        return costs;
    }

    // West-to-east along 36.5 N through the restricted block.
    const geo_point WEST{36.5, -101.0};
    const geo_point EAST{36.5, -98.0};

    const elevation_source& no_terrain()
    {
        static const elevation_source terrain("missing-terrain-tree");
        return terrain;
    }

    // One SUA of `type` with a single stratum over the square 36..36.1 N,
    // 100..99.9 W.
    sua one_stratum_sua(const std::string& type, int lower, const std::string& lower_ref, int upper,
                        const std::string& upper_ref)
    {
        sua s{};
        s.sua_type = type;
        s.name = "TEST " + type;
        sua_stratum stratum;
        stratum.lower_ft_val = lower;
        stratum.lower_ft_ref = lower_ref;
        stratum.upper_ft_val = upper;
        stratum.upper_ft_ref = upper_ref;
        sua_ring ring;
        ring.points = square(-100.0, 36.0, -99.9, 36.1).points;
        stratum.parts.push_back(ring);
        s.strata.push_back(stratum);
        return s;
    }
}

TEST_CASE("a leg through a rejected volume reports it")
{
    const airspace_index index({restricted_block()});
    const auto hit = index.costliest_crossing(WEST, EAST, 5000.0, rejecting_restricted(), {});
    CHECK(hit.cost == cost_reject);
    REQUIRE(hit.volume != nullptr);
    CHECK(hit.volume->name == "R-TEST");
}

TEST_CASE("a leg passing beside a volume crosses nothing")
{
    const airspace_index index({restricted_block()});
    const auto hit = index.costliest_crossing({35.5, -101.0}, {35.5, -98.0}, 5000.0, rejecting_restricted(), {});
    CHECK(hit.cost == cost_include);
    CHECK(hit.volume == nullptr);
}

TEST_CASE("a leg ending inside a volume crosses it")
{
    const airspace_index index({restricted_block()});
    CHECK(index.costliest_crossing(WEST, {36.5, -99.5}, 5000.0, rejecting_restricted(), {}).volume != nullptr);
}

TEST_CASE("cruise below the floor or above the ceiling crosses nothing")
{
    const airspace_index index({restricted_block()});
    CHECK(index.costliest_crossing(WEST, EAST, 1999.0, rejecting_restricted(), {}).volume == nullptr);
    CHECK(index.costliest_crossing(WEST, EAST, 10001.0, rejecting_restricted(), {}).volume == nullptr);
}

TEST_CASE("cruise at the floor or the ceiling crosses the volume")
{
    const airspace_index index({restricted_block()});
    CHECK(index.costliest_crossing(WEST, EAST, 2000.0, rejecting_restricted(), {}).volume != nullptr);
    CHECK(index.costliest_crossing(WEST, EAST, 10000.0, rejecting_restricted(), {}).volume != nullptr);
}

TEST_CASE("a leg inside a hole crosses nothing")
{
    auto donut = restricted_block();
    donut.rings.push_back(square(-99.8, 36.2, -99.2, 36.8, true));
    const airspace_index index({donut});
    CHECK(index.costliest_crossing({36.5, -99.7}, {36.5, -99.3}, 5000.0, rejecting_restricted(), {}).volume ==
          nullptr);
}

TEST_CASE("an exempt volume is skipped")
{
    const airspace_index index({restricted_block()});
    CHECK(index.costliest_crossing(WEST, EAST, 5000.0, rejecting_restricted(), {0}).volume == nullptr);
}

TEST_CASE("an included class is not reported")
{
    auto moa = restricted_block();
    moa.kind = airspace_class::moa;
    const airspace_index index({moa});
    CHECK(index.costliest_crossing(WEST, EAST, 5000.0, rejecting_restricted(), {}).volume == nullptr);
}

TEST_CASE("the costliest of several crossed volumes is reported")
{
    airspace_volume warning{airspace_class::warning, "W-TEST", 0.0, 20000.0, {square(-100.5, 36.0, -100.2, 37.0)}};
    const airspace_index index({warning, restricted_block()});
    const auto hit = index.costliest_crossing(WEST, EAST, 5000.0, rejecting_restricted(), {});
    CHECK(hit.cost == cost_reject);
    REQUIRE(hit.volume != nullptr);
    CHECK(hit.volume->name == "R-TEST");
}

TEST_CASE("an avoided volume alone is reported at its cost")
{
    airspace_volume warning{airspace_class::warning, "W-TEST", 0.0, 20000.0, {square(-100.5, 36.0, -100.2, 37.0)}};
    const airspace_index index({warning});
    CHECK(index.costliest_crossing(WEST, EAST, 5000.0, rejecting_restricted(), {}).cost == cost_avoid);
}

TEST_CASE("a leg across the antimeridian crosses a volume on the far side")
{
    airspace_volume far{airspace_class::restricted, "R-FAR", 0.0, 20000.0, {square(-179.9, 50.0, -179.6, 51.0)}};
    const airspace_index index({far});
    CHECK(index.costliest_crossing({50.5, 179.5}, {50.5, -179.5}, 5000.0, rejecting_restricted(), {}).volume !=
          nullptr);
}

TEST_CASE("containing finds the volumes over a point")
{
    const airspace_index index({restricted_block()});
    CHECK(index.containing({36.5, -99.5}) == std::vector<std::size_t>{0});
    CHECK(index.containing({38.0, -99.5}).empty());
}

TEST_CASE("sua_volumes keeps MSL limits and maps the SUA type")
{
    const auto volumes = sua_volumes({one_stratum_sua("PA", 0, "SFC", 3000, "MSL")}, no_terrain());
    REQUIRE(volumes.size() == 1);
    CHECK(volumes[0].kind == airspace_class::prohibited);
    CHECK(volumes[0].name == "TEST PA");
    CHECK(volumes[0].lower_ft == 0.0);
    CHECK(volumes[0].upper_ft == 3000.0);
}

TEST_CASE("sua_volumes makes an unlimited ceiling unlimited")
{
    const auto volumes = sua_volumes({one_stratum_sua("RA", 0, "SFC", 99999, "OTHER")}, no_terrain());
    CHECK(volumes.at(0).upper_ft == altitude_filter::UNLIMITED_FT);
}

TEST_CASE("sua_volumes makes a surface-referenced ceiling unlimited without terrain")
{
    const auto volumes = sua_volumes({one_stratum_sua("RA", 0, "SFC", 2000, "SFC")}, no_terrain());
    CHECK(volumes.at(0).upper_ft == altitude_filter::UNLIMITED_FT);
}

TEST_CASE("sua_volumes raises a surface-referenced ceiling by the terrain under it")
{
    constexpr int zoom = 10;
    const test::temporary_tree tree("airspace-terrain", test::manifest("airspace", 1, 0, zoom, zoom, 1.0, "EGM2008",
                                                                       {}, false, "[-180, -85, 180, 85]"));
    const auto key = address_elevation(36.05, -99.95, zoom, 1, 0).key;
    test::write_elevation_tile_m(tree.path() / std::to_string(zoom) / std::to_string(key.x) /
                                     (std::to_string(key.y) + ".png"),
                                 1000.0);
    const elevation_source terrain(tree.path());
    const auto ground_ft = terrain.maximum_elevation_ft(36.0, -100.0, 36.1, -99.9);
    REQUIRE(ground_ft.has_value());

    const auto volumes = sua_volumes({one_stratum_sua("RA", 0, "SFC", 2000, "SFC")}, terrain);
    CHECK(volumes.at(0).upper_ft == 2000.0 + *ground_ft);
}

TEST_CASE("sua_volumes yields one volume per stratum")
{
    auto s = one_stratum_sua("MOA", 500, "MSL", 9000, "MSL");
    s.strata.push_back(s.strata.front());
    CHECK(sua_volumes({s}, no_terrain()).size() == 2);
}

TEST_CASE("tfr_volumes yields one volume per area named by the NOTAM")
{
    tfr t{};
    t.notam_id = "6/1234";
    tfr_area area{};
    area.lower_ft_val = 0;
    area.lower_ft_ref = "SFC";
    area.upper_ft_val = 18000;
    area.upper_ft_ref = "MSL";
    area.points = square(-100.0, 36.0, -99.9, 36.1).points;
    t.areas = {area, area};
    const auto volumes = tfr_volumes({t}, no_terrain());
    REQUIRE(volumes.size() == 2);
    CHECK(volumes[0].kind == airspace_class::tfr);
    CHECK(volumes[0].name == "6/1234");
    CHECK(volumes[0].upper_ft == 18000.0);
}
