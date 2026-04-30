#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "data_source.hpp"

#include <chrono>

using osect::data_source;
using osect::data_source_status;

namespace
{
    // Howard Hinnant's civil-to-serial-day algorithm, inlined here so
    // the test can build canonical UTC time_points without depending on
    // any production-side date helper. Days counted from 1970-01-01.
    constexpr int days_from_civil(int y, unsigned m, unsigned d) noexcept
    {
        y -= m <= 2;
        const int era = (y >= 0 ? y : y - 399) / 400;
        const unsigned yoe = static_cast<unsigned>(y - era * 400);
        const unsigned doy =
            (153U * (m + (m > 2 ? -3U : 9U)) + 2U) / 5U + d - 1U;
        const unsigned doe = yoe * 365U + yoe / 4U - yoe / 100U + doy;
        return era * 146097 + static_cast<int>(doe) - 719468;
    }

    std::chrono::system_clock::time_point make_utc(int y, int m, int d)
    {
        return std::chrono::system_clock::time_point(
            std::chrono::seconds(
                static_cast<long long>(days_from_civil(
                    y, static_cast<unsigned>(m), static_cast<unsigned>(d)))
                * 86400LL));
    }
}

TEST_CASE("status: fresh before expiry, expired at and after")
{
    data_source s;
    s.expires = make_utc(2026, 5, 14);

    CHECK(s.status_at(make_utc(2026, 4, 30)) == data_source_status::fresh);
    CHECK(s.status_at(make_utc(2026, 5, 14)) == data_source_status::expired);
    CHECK(s.status_at(make_utc(2026, 5, 15)) == data_source_status::expired);
}

TEST_CASE("status: unknown when expiry is absent")
{
    data_source s;
    // No `expires` set — registry has no opinion.
    CHECK(s.status_at(make_utc(2099, 1, 1)) == data_source_status::unknown);
}
