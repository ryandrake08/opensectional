#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "ephemeral_cache.hpp"
#include "http_client.hpp"
#include "tfr_source.hpp"

#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

using osect::airspace_point;
using osect::ephemeral_cache;
using osect::http_client;
using osect::tfr;
using osect::tfr_area;
using osect::tfr_source;

namespace osect
{
    // Test-only handle into the file-private serializer; mirrors the
    // shape tfr_source uses to write its cache. Declared in
    // tfr_source.cpp's anonymous namespace and surfaced via this
    // single linkable function so tests can populate the cache
    // without dragging in the live-fetch path.
    std::string tfr_source_serialize_for_test(const std::vector<tfr>& v);
}

namespace
{
    struct tmp_dir
    {
        std::filesystem::path path;
        explicit tmp_dir(const char* tag)
        {
            const auto base = std::filesystem::temp_directory_path() /
                ("osect_tfr_source_test_" + std::string(tag) + "_XXXXXX");
            for(int i = 0; i < 1000; ++i)
            {
                const auto candidate = base.string() + std::to_string(i);
                if(!std::filesystem::exists(candidate))
                {
                    path = candidate;
                    return;
                }
            }
            throw std::runtime_error("could not pick a tmp dir");
        }
        ~tmp_dir()
        {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }
        tmp_dir(const tmp_dir&) = delete;
        tmp_dir& operator=(const tmp_dir&) = delete;
    };

    tfr make_test_tfr()
    {
        tfr t{};
        t.tfr_id = 7;
        t.notam_id = "9/9999";
        t.tfr_type = "99.7";
        t.facility = "ZOA";
        t.date_expire = "2099-01-01T00:00:00";
        t.description = "synthetic test fixture";
        tfr_area a{};
        a.area_id = 1;
        a.area_name = "Area A";
        a.upper_ft_val = 1000;
        a.upper_ft_ref = "MSL";
        a.lower_ft_val = 0;
        a.lower_ft_ref = "SFC";
        // Tiny triangle.
        a.points.push_back(airspace_point{37.0, -122.0});
        a.points.push_back(airspace_point{37.1, -122.0});
        a.points.push_back(airspace_point{37.0, -121.9});
        t.areas.push_back(std::move(a));
        return t;
    }
}

TEST_CASE("tfr_source: empty cache yields empty store")
{
    tmp_dir dir("empty");
    ephemeral_cache cache(dir.path);
    http_client http(/*offline=*/true);

    tfr_source src(http, cache);
    CHECK(src.snapshot().empty());
    CHECK(src.snapshot_segments().empty());
    CHECK(!src.last_updated().has_value());
}

TEST_CASE("tfr_source: populated cache loads on construction")
{
    tmp_dir dir("loaded");
    ephemeral_cache cache(dir.path);
    http_client http(/*offline=*/true);

    // Pre-populate the cache with a serialized state via the
    // test-only helper, mirroring what tfr_source itself would write.
    const auto fixture = make_test_tfr();
    cache.store("tfr",
        osect::tfr_source_serialize_for_test({fixture}), "");

    tfr_source src(http, cache);
    auto snap = src.snapshot();
    REQUIRE(snap.size() == 1);
    CHECK(snap[0].notam_id == "9/9999");
    CHECK(snap[0].facility == "ZOA");
    REQUIRE(snap[0].areas.size() == 1);
    CHECK(snap[0].areas[0].upper_ft_val == 1000);
    CHECK(snap[0].areas[0].points.size() == 3);

    // Segments are built on load too.
    auto segs = src.snapshot_segments();
    REQUIRE(!segs.empty());
    CHECK(segs[0].upper_ft_val == 1000);
    CHECK(segs[0].lower_ft_ref == "SFC");
}

TEST_CASE("tfr_source: refresh in offline mode is a safe no-op")
{
    tmp_dir dir("offline");
    ephemeral_cache cache(dir.path);
    http_client http(/*offline=*/true);

    const auto fixture = make_test_tfr();
    cache.store("tfr",
        osect::tfr_source_serialize_for_test({fixture}), "");

    tfr_source src(http, cache);
    REQUIRE(src.snapshot().size() == 1);

    // Offline refresh should swallow the http error and leave the
    // existing in-memory store intact.
    src.refresh();
    CHECK(src.snapshot().size() == 1);
    CHECK(src.snapshot()[0].notam_id == "9/9999");
}

TEST_CASE("tfr_source: corrupt cache loads as empty, no crash")
{
    tmp_dir dir("corrupt");
    ephemeral_cache cache(dir.path);
    http_client http(/*offline=*/true);

    cache.store("tfr", "definitely not a TFRC blob", "");
    tfr_source src(http, cache);
    CHECK(src.snapshot().empty());
}
