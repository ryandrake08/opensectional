#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "hex_color.hpp"

TEST_CASE("#RRGGBB parses each channel and leaves alpha opaque")
{
    const auto c = osect::parse_hex_color("#3366cc");
    REQUIRE(c);
    CHECK(c->r == 0x33 / 255.0F);
    CHECK(c->g == 0x66 / 255.0F);
    CHECK(c->b == 0xCC / 255.0F);
    CHECK(c->a == 1.0F);
    CHECK_FALSE(c->has_alpha);
}

TEST_CASE("#RGB repeats each digit")
{
    const auto c = osect::parse_hex_color("#1aF");
    REQUIRE(c);
    CHECK(c->r == 0x11 / 255.0F);
    CHECK(c->g == 0xAA / 255.0F);
    CHECK(c->b == 1.0F);
    CHECK_FALSE(c->has_alpha);
}

TEST_CASE("#RRGGBBAA parses alpha")
{
    const auto c = osect::parse_hex_color("#00000080");
    REQUIRE(c);
    CHECK(c->r == 0.0F);
    CHECK(c->a == 0x80 / 255.0F);
    CHECK(c->has_alpha);
}

TEST_CASE("malformed hex colors are rejected")
{
    CHECK_FALSE(osect::parse_hex_color(""));
    CHECK_FALSE(osect::parse_hex_color("#"));
    CHECK_FALSE(osect::parse_hex_color("336699"));
    CHECK_FALSE(osect::parse_hex_color("#12"));
    CHECK_FALSE(osect::parse_hex_color("#12345"));
    CHECK_FALSE(osect::parse_hex_color("#1234567"));
    CHECK_FALSE(osect::parse_hex_color("#12g"));
    CHECK_FALSE(osect::parse_hex_color("navy"));
}

TEST_CASE("an invalid low digit in a channel pair is rejected")
{
    CHECK_FALSE(osect::parse_hex_color("#1g1g1g"));
    CHECK_FALSE(osect::parse_hex_color("#1122331g"));
}
