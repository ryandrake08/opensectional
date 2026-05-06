#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "altitude_filter.hpp"

using namespace osect;

TEST_CASE("default filter shows low band only")
{
    altitude_filter af;
    CHECK(af.show_low);
    CHECK_FALSE(af.show_high);
    CHECK(af.any());
}

TEST_CASE("overlaps(int,int) with low-band-only")
{
    altitude_filter af;
    af.show_low = true;
    af.show_high = false;

    CHECK(af.overlaps(0, 5000));                     // entirely low
    CHECK(af.overlaps(0, altitude_filter::DIVIDER_FT)); // crosses divider
    CHECK_FALSE(af.overlaps(20000, 30000));          // entirely high
    CHECK(af.overlaps(altitude_filter::DIVIDER_FT - 1, 99999)); // touches low
}

TEST_CASE("overlaps(int,int) with high-band-only")
{
    altitude_filter af;
    af.show_low = false;
    af.show_high = true;

    CHECK_FALSE(af.overlaps(0, 5000));               // entirely low
    CHECK(af.overlaps(20000, 30000));                // entirely high
    CHECK(af.overlaps(10000, 25000));                // crosses divider
    CHECK(af.overlaps(altitude_filter::DIVIDER_FT, altitude_filter::DIVIDER_FT));
}

TEST_CASE("overlaps(int,int) with both bands disabled")
{
    altitude_filter af;
    af.show_low = false;
    af.show_high = false;

    CHECK_FALSE(af.any());
    CHECK_FALSE(af.overlaps(0, 5000));
    CHECK_FALSE(af.overlaps(20000, 30000));
    CHECK_FALSE(af.overlaps(0, 99999));
}

TEST_CASE("overlaps with both bands enabled shows everything")
{
    altitude_filter af;
    af.show_low = true;
    af.show_high = true;

    CHECK(af.overlaps(0, 5000));
    CHECK(af.overlaps(20000, 30000));
    CHECK(af.overlaps(10000, 25000));
}

TEST_CASE("reference-aware overlaps: SFC lower bound always reaches low")
{
    altitude_filter af;
    af.show_low = true;
    af.show_high = false;

    // AGL surface bound — classified as low regardless of numeric value
    CHECK(af.overlaps(0, "SFC", 10000, "MSL"));
    CHECK(af.overlaps(500, "SFC", 17999, "MSL"));
}

TEST_CASE("reference-aware overlaps: SFC upper bound never reaches high")
{
    altitude_filter af;
    af.show_low = false;
    af.show_high = true;

    // SFC ceilings — never classified as reaching high band
    CHECK_FALSE(af.overlaps(0, "SFC", 5000, "SFC"));
}

TEST_CASE("reference-aware overlaps: OTHER is unbounded (both bands)")
{
    altitude_filter af_low;
    af_low.show_low = true;
    af_low.show_high = false;
    CHECK(af_low.overlaps(0, "OTHER", 99999, "OTHER"));

    altitude_filter af_high;
    af_high.show_low = false;
    af_high.show_high = true;
    CHECK(af_high.overlaps(0, "OTHER", 99999, "OTHER"));
}

TEST_CASE("reference-aware overlaps: STD treated like MSL at divider")
{
    altitude_filter af;
    af.show_low = false;
    af.show_high = true;

    CHECK(af.overlaps(18000, "STD", 60000, "STD"));
    CHECK_FALSE(af.overlaps(10000, "MSL", 17999, "MSL"));
}

TEST_CASE("artcc_bands: LOW / HIGH / UNLIMITED are three distinct bands")
{
    CHECK(artcc_bands("LOW")       == 0b001);
    CHECK(artcc_bands("HIGH")      == 0b010);
    CHECK(artcc_bands("UNLIMITED") == 0b100);
}

TEST_CASE("altitude_filter_allows: mask ∩ filter")
{
    altitude_filter af;
    af.show_low = true;
    af.show_high = false;

    CHECK(altitude_filter_allows(af, 0b01));         // low ∩ low-only
    CHECK(altitude_filter_allows(af, 0b11));         // both ∩ low-only
    CHECK_FALSE(altitude_filter_allows(af, 0b10));   // high ∩ low-only

    af.show_low = false;
    af.show_high = true;
    CHECK_FALSE(altitude_filter_allows(af, 0b01));
    CHECK(altitude_filter_allows(af, 0b10));

    af.show_low = false;
    af.show_high = false;
    CHECK_FALSE(altitude_filter_allows(af, 0b11));   // no bands on → nothing allowed
}

TEST_CASE("equality operators")
{
    altitude_filter a;
    altitude_filter b;
    CHECK(a == b);
    CHECK_FALSE(a != b);

    b.show_high = true;
    CHECK(a != b);
    CHECK_FALSE(a == b);
}
