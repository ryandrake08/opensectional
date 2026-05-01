#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "http_client.hpp"

#include <chrono>
#include <stdexcept>

using osect::http_client;

// libcurl exercises its own GET / TLS / encoding paths in its test
// suite; we only need to confirm that *our* wrapper translates a
// transport failure into a thrown exception the rest of the app can
// catch and fall back from. Real-network round-trips (200, 304, gzip)
// are exercised by the first ephemeral source that consumes
// http_client (TFR — see `Stage 3` in IMPLEMENTATION_PLAN).
TEST_CASE("http_client: connection refused throws")
{
    http_client client;
    http_client::request r;
    // Port 1 is privileged and unbound on every dev machine; connect
    // refused fires immediately so a long timeout isn't needed.
    r.url = "http://127.0.0.1:1/";
    r.timeout = std::chrono::seconds{2};
    CHECK_THROWS_AS(client.get(r), std::runtime_error);
}
