#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "airspace_index.hpp"
#include "elevation_source.hpp"
#include "ephemeral_database.hpp"
#include "flight_route.hpp"
#include "nasr_database.hpp"
#include "route_planner.hpp"
#include "tmp_user_db.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

using namespace osect;

namespace
{
    const nasr_database& test_db()
    {
        static const nasr_database db("osect.db");
        return db;
    }

    const elevation_source& no_terrain()
    {
        static const elevation_source terrain("missing-terrain-tree");
        return terrain;
    }

    // Options avoiding airspace at `cruise_ft`: prohibited, restricted,
    // and TFR REJECT, every other class INCLUDE.
    route_planner::options avoiding(std::optional<double> cruise_ft)
    {
        route_planner::options opts;
        opts.avoid_airspace = true;
        opts.cruise_altitude_ft = cruise_ft;
        for(const auto c : {airspace_class::prohibited, airspace_class::restricted, airspace_class::tfr})
        {
            opts.airspace_cost.at(static_cast<std::size_t>(c)) = cost_reject;
        }
        return opts;
    }

    // `opts` with airspace avoidance off.
    route_planner::options ignoring_airspace(route_planner::options opts)
    {
        opts.avoid_airspace = false;
        return opts;
    }

    const airspace_index& sua_index()
    {
        static const airspace_index index(sua_volumes(test_db().query_sua({-180.0, -90.0, 180.0, 90.0}), no_terrain()));
        return index;
    }

    // Names of the volumes in `index` at cost_reject under `opts` that a
    // leg of `text` crosses at cruise, ignoring the volumes over its
    // first and last waypoints.
    std::vector<std::string> rejected_crossings(const std::string& text, const airspace_index& index,
                                                const route_planner::options& opts)
    {
        const flight_route route(text, test_db(), {});
        const auto& points = route.waypoints;
        auto exempt = index.containing({points.front().lat, points.front().lon});
        const auto at_end = index.containing({points.back().lat, points.back().lon});
        exempt.insert(exempt.end(), at_end.begin(), at_end.end());
        std::vector<std::string> names;
        for(std::size_t i = 0; i + 1 < points.size(); ++i)
        {
            const auto c = index.costliest_crossing({points[i].lat, points[i].lon},
                                                    {points[i + 1].lat, points[i + 1].lon}, *opts.cruise_altitude_ft,
                                                    opts.airspace_cost, exempt);
            if(c.cost >= cost_reject)
            {
                names.push_back(c.volume->name);
            }
        }
        return names;
    }

    const route_planner& sua_planner()
    {
        static test::tmp_user_db dbs("airspace_sua");
        static const route_planner planner("osect.db", dbs.db_file, dbs.ephemeral_db_file, no_terrain());
        return planner;
    }

    // A lon/lat-aligned rectangle.
    std::vector<geo_point> rectangle(double lon0, double lat0, double lon1, double lat1)
    {
        return {{lat0, lon0}, {lat0, lon1}, {lat1, lon1}, {lat1, lon0}};
    }

    // A one-area TFR from the surface to `upper_msl_ft` over `points`.
    tfr make_tfr(int id, const std::string& notam_id, int upper_msl_ft, std::vector<geo_point> points)
    {
        tfr t{};
        t.tfr_id = id;
        t.notam_id = notam_id;
        tfr_area area{};
        area.area_id = 1;
        area.lower_ft_val = 0;
        area.lower_ft_ref = "SFC";
        area.upper_ft_val = upper_msl_ft;
        area.upper_ft_ref = "MSL";
        area.points = std::move(points);
        t.areas.push_back(area);
        return t;
    }

    // A wall across the KTRM-KBLH direct line, surface to 3,000 ft MSL.
    const std::string WALL_NOTAM = "9/0001";
    constexpr double WALL_TOP_FT = 3000.0;

    // A square ring around KIPL with a slit to its inner square, so
    // every route into KIPL crosses it while KIPL lies outside it.
    const std::string KEYHOLE_NOTAM = "9/0002";

    std::vector<geo_point> keyhole_around_kipl()
    {
        constexpr double lat = 32.834;
        constexpr double lon = -115.579;
        constexpr double outer = 0.3;
        constexpr double inner = 0.1;
        return {{lat - outer, lon},         {lat - outer, lon + outer}, {lat + outer, lon + outer},
                {lat + outer, lon - outer}, {lat - outer, lon - outer}, {lat - outer, lon},
                {lat - inner, lon},         {lat - inner, lon - inner}, {lat + inner, lon - inner},
                {lat + inner, lon + inner}, {lat - inner, lon + inner}, {lat - inner, lon}};
    }

    std::vector<tfr> test_tfrs()
    {
        return {make_tfr(1, WALL_NOTAM, static_cast<int>(WALL_TOP_FT), rectangle(-115.5, 33.3, -115.4, 33.9)),
                make_tfr(2, KEYHOLE_NOTAM, 18000, keyhole_around_kipl())};
    }

    // Writes `tfrs` to the ephemeral database at `path` and returns it.
    const std::filesystem::path& with_tfrs(const std::filesystem::path& path, const std::vector<tfr>& tfrs)
    {
        ephemeral_database(path).replace_tfrs(tfrs);
        return path;
    }

    const route_planner& tfr_planner()
    {
        static test::tmp_user_db dbs("airspace_tfr");
        static const route_planner planner("osect.db", dbs.db_file, with_tfrs(dbs.ephemeral_db_file, test_tfrs()),
                                           no_terrain());
        return planner;
    }

    const airspace_index& tfr_index()
    {
        static const airspace_index index(tfr_volumes(test_tfrs(), no_terrain()));
        return index;
    }

    // The expansion's error message, or empty when it plans.
    std::string expansion_error(const route_planner& planner, const std::string& text,
                                const route_planner::options& opts)
    {
        try
        {
            planner.expand_sigils(text, opts);
            return {};
        }
        catch(const route_parse_error& e)
        {
            return e.what();
        }
    }
}

TEST_CASE("airspace avoidance needs a cruise altitude")
{
    CHECK(expansion_error(sua_planner(), "KTPH ? KLAS", avoiding(std::nullopt)) ==
          "airspace avoidance needs a cruise altitude");
}

TEST_CASE("the planned route crosses restricted areas when not avoiding them")
{
    const auto opts = avoiding(9500.0);
    const auto text = sua_planner().expand_sigils("KTPH ? KLAS", ignoring_airspace(opts)).text;
    CHECK_FALSE(rejected_crossings(text, sua_index(), opts).empty());
}

TEST_CASE("the planned route avoids rejected restricted areas")
{
    const auto opts = avoiding(9500.0);
    const auto text = sua_planner().expand_sigils("KTPH ? KLAS", opts).text;
    CAPTURE(text);
    CHECK(rejected_crossings(text, sua_index(), opts).empty());
}

TEST_CASE("included restricted areas are not avoided")
{
    auto opts = avoiding(9500.0);
    opts.airspace_cost.at(static_cast<std::size_t>(airspace_class::restricted)) = cost_include;
    CHECK(sua_planner().expand_sigils("KTPH ? KLAS", opts).text ==
          sua_planner().expand_sigils("KTPH ? KLAS", ignoring_airspace(opts)).text);
}

TEST_CASE("a route plans out of the restricted area its origin lies in")
{
    // 37°03' N 116°00' W lies inside R-4808N.
    REQUIRE_FALSE(sua_index().containing({37.05, -116.0}).empty());
    CHECK(expansion_error(sua_planner(), "370300N1160000W ? KLAS", avoiding(9500.0)).empty());
}

TEST_CASE("the planned route avoids a TFR at cruise")
{
    const auto opts = avoiding(WALL_TOP_FT - 500.0);
    const auto text = tfr_planner().expand_sigils("KTRM ? KBLH", opts).text;
    CAPTURE(text);
    CHECK(text != "KTRM KBLH");
    CHECK(rejected_crossings(text, tfr_index(), opts).empty());
}

TEST_CASE("the planned route flies over a TFR below cruise")
{
    CHECK(tfr_planner().expand_sigils("KTRM ? KBLH", avoiding(WALL_TOP_FT + 500.0)).text == "KTRM KBLH");
}

TEST_CASE("a segment with no route around rejected airspace names it")
{
    const auto error = expansion_error(tfr_planner(), "KTRM ? KIPL", avoiding(9500.0));
    CAPTURE(error);
    CHECK(error.find("blocked by") != std::string::npos);
    CHECK(error.find(KEYHOLE_NOTAM) != std::string::npos);
}
