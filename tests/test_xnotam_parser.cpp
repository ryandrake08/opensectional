#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "ephemeral_database.hpp"
#include "xnotam_parser.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

using osect::parse_xnotam;
using osect::tfr;
using osect::xnotam_parse_error;

namespace
{
    // Read a fixture XML from tests/fixtures/, relative to the ctest
    // WORKING_DIRECTORY (the repo root). The fixture is checked into
    // the repo so the test runs without a populated nasr_data/.
    std::string read_fixture(const std::string& name)
    {
        const auto path = std::filesystem::path("tests/fixtures") / name;
        std::ifstream in(path);
        REQUIRE(in.is_open());
        std::stringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }
}

// detail_6_4045.xml is the FAA's "Ann Arbor TEST TEST TEST" XNOTAM —
// one TFRAreaGroup with a 38-vertex pre-tessellated polygon, an HEI
// (AGL) altitude band of 0 to 400 ft, and the codeType 99.7 (Special
// Security Instructions). Golden values cross-checked against
// `tools/build_tfr.py` running over the same fixture directory.
TEST_CASE("parse_xnotam: Ann Arbor test TFR populates expected fields")
{
    const auto xml = read_fixture("xnotam_detail_6_4045.xml");
    const auto result = parse_xnotam(xml);
    REQUIRE(result.has_value());

    const tfr& t = *result;
    CHECK(t.notam_id == "TEST TEST TEST TFR");
    CHECK(t.tfr_type == "99.7");
    CHECK(t.facility == "ZOB");
    CHECK(t.date_expire == "2026-04-18T09:00:00");
    CHECK(!t.description.empty());
    CHECK(t.date_issued == "2026-04-18T02:08:00");
    CHECK(t.city == "TEST TEST TEST Ann Arbor");
    CHECK(t.state == "MICHIGAN");
    CHECK(t.coord_facility_type == "ARTCC");
    CHECK(t.coord_facility.empty());
    CHECK(t.coord_facility_name.empty());
    CHECK(t.coord_phone.empty());
    CHECK(t.coord_freq.empty());
    CHECK(t.poc_name.empty());
    CHECK(t.time_zone == "EDT");
    CHECK(t.expire_time_zone == "EDT");

    REQUIRE(t.areas.size() == 1);
    const auto& a = t.areas[0];
    CHECK(a.upper_ft_val == 400);
    CHECK(a.upper_ft_ref == "SFC");
    CHECK(a.lower_ft_val == 0);
    CHECK(a.lower_ft_ref == "SFC");
    CHECK(a.date_expire == "2026-04-18T09:00:00");
    CHECK(a.is_time_separate == "FALSE");
    CHECK(a.start_time.empty());
    CHECK(a.end_time.empty());
    CHECK(a.day_code.empty());
    REQUIRE(!a.instructions.empty());
    CHECK(a.instructions.find("THIS IS A TEST TFR") == 0);
    CHECK(a.instructions.find('\n') != std::string::npos);

    // 38 Avx points in the merged polygon (one repeats to close).
    REQUIRE(a.points.size() >= 3);
    // First vertex matches the literal value in the fixture XML.
    CHECK(a.points.front().lon == doctest::Approx(-83.74861111));
    CHECK(a.points.front().lat == doctest::Approx(42.27416977));
}

TEST_CASE("parse_xnotam: malformed XML throws")
{
    CHECK_THROWS_AS(parse_xnotam("<not closed"), xnotam_parse_error);
    CHECK_THROWS_AS(parse_xnotam(""), xnotam_parse_error);
}

TEST_CASE("parse_xnotam: well-formed but wrong-shape returns nullopt")
{
    // Valid XML, but the envelope isn't an XNOTAM-Update document.
    const std::string other =
        "<?xml version=\"1.0\"?><Other><Stuff/></Other>";
    CHECK(!parse_xnotam(other).has_value());
}

TEST_CASE("parse_xnotam: TFRAreaGroup with no polygon returns nullopt")
{
    // Strip abdMergedArea out of the test fixture so the parser sees
    // a notam with no plottable geometry. Using a substring-replace
    // keeps the rest of the document well-formed.
    auto xml = read_fixture("xnotam_detail_6_4045.xml");
    const auto open = xml.find("<abdMergedArea>");
    const auto close = xml.find("</abdMergedArea>");
    REQUIRE(open != std::string::npos);
    REQUIRE(close != std::string::npos);
    xml.erase(open, (close - open) + std::string("</abdMergedArea>").size());

    CHECK(!parse_xnotam(xml).has_value());
}
