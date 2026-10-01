#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "route_submitter.hpp"
#include "tmp_user_db.hpp"

#include <thread>

namespace osect
{
    void wake_main_thread()
    {
    }
}

namespace
{
    osect::route_status wait_for_completion(osect::route_submitter& submitter)
    {
        osect::route_status status;
        do
        {
            status = submitter.poll();
            std::this_thread::yield();
        } while(status.pending);
        return status;
    }
}

TEST_CASE("route submitter delivers a resubmission and never the plan it replaced")
{
    osect::test::tmp_user_db user_db("submitter_resubmit");
    osect::route_submitter submitter("osect.db", user_db.db_file);

    // A cross-country plan, replaced while it may still be running.
    submitter.submit("KSEA ? KMIA", {}, 1);
    submitter.submit("KSMF ? KBFL", {}, 2);

    const auto status = wait_for_completion(submitter);
    REQUIRE(status.completion);
    CHECK(status.completion->tag == 2);
    CHECK(status.completion->error.empty());
    CHECK(status.completion->route);

    const auto idle = submitter.poll();
    CHECK_FALSE(idle.pending);
    CHECK_FALSE(idle.completion);
}
