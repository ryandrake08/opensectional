#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "lru_set.hpp"

#include <stdexcept>
#include <string>

TEST_CASE("empty set reports size zero and nothing exists")
{
    lru_set<int> s(3);
    CHECK(s.size() == 0);
    CHECK_FALSE(s.exists(42));
}

TEST_CASE("put adds items and exists/size reflect them")
{
    lru_set<int> s(3);
    s.put(1);
    s.put(2);
    s.put(3);
    CHECK(s.size() == 3);
    CHECK(s.exists(1));
    CHECK(s.exists(2));
    CHECK(s.exists(3));
}

TEST_CASE("get on missing value throws range_error")
{
    lru_set<int> s(3);
    s.put(1);
    CHECK_THROWS_AS(s.get(999), std::range_error);
}

TEST_CASE("exceeding max_size evicts the least-recently-used entry")
{
    lru_set<int> s(3);
    s.put(1);
    s.put(2);
    s.put(3);
    s.put(4);    // evicts 1

    CHECK(s.size() == 3);
    CHECK_FALSE(s.exists(1));
    CHECK(s.exists(2));
    CHECK(s.exists(3));
    CHECK(s.exists(4));
}

TEST_CASE("get() promotes item to most-recently-used")
{
    lru_set<int> s(3);
    s.put(1);
    s.put(2);
    s.put(3);

    // Touch 1 — now eviction order should be 2, 3, 1 (oldest first)
    (void)s.get(1);
    s.put(4);    // evicts 2

    CHECK(s.exists(1));
    CHECK_FALSE(s.exists(2));
    CHECK(s.exists(3));
    CHECK(s.exists(4));
}

TEST_CASE("put() on existing item moves it to front without growing size")
{
    lru_set<int> s(3);
    s.put(1);
    s.put(2);
    s.put(3);
    s.put(1);    // re-insert — 1 becomes MRU, size unchanged

    CHECK(s.size() == 3);

    s.put(4);    // evicts 2 (now oldest), not 1
    CHECK(s.exists(1));
    CHECK_FALSE(s.exists(2));
    CHECK(s.exists(3));
    CHECK(s.exists(4));
}

TEST_CASE("works with non-trivial value types")
{
    lru_set<std::string> s(2);
    s.put("alpha");
    s.put("beta");
    CHECK(s.exists("alpha"));
    CHECK(s.get("alpha") == "alpha");

    s.put("gamma");    // evicts "beta" (alpha was just touched)
    CHECK(s.exists("alpha"));
    CHECK_FALSE(s.exists("beta"));
    CHECK(s.exists("gamma"));
}
