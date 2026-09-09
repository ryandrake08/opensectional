#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "elevation_source.hpp"
#include "terrain_profile_worker.hpp"

#include <thread>

namespace osect
{
    void wake_main_thread()
    {
    }
}

TEST_CASE("terrain profile worker delivers an asynchronous result")
{
    const osect::elevation_source terrain("missing-terrain-tree");
    osect::terrain_profile_worker worker(terrain, "osect.db");
    worker.submit({{osect::waypoint_kind::airport, "O61", 38.684, -120.98752777},
                   {osect::waypoint_kind::airport, "KMER", 37.38048444, -120.56818638}},
                  10000.0);

    osect::terrain_profile_status status;
    do
    {
        status = worker.poll();
        std::this_thread::yield();
    } while(status.pending);

    REQUIRE(status.error.empty());
    REQUIRE(status.result);
    CHECK_FALSE(status.result->samples.empty());
}
