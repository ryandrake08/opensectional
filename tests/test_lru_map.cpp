#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "lru_map.hpp"

#include <string>

TEST_CASE("lru_map::find returns null for a missing key")
{
    osect::lru_map<int, std::string> map(2);

    CHECK(map.find(1) == nullptr);
}

TEST_CASE("lru_map::find returns the stored value")
{
    osect::lru_map<int, std::string> map(2);
    map.put(1, "one");

    REQUIRE(map.find(1) != nullptr);
    CHECK(*map.find(1) == "one");
}

TEST_CASE("lru_map evicts the least recently used entry beyond capacity")
{
    osect::lru_map<int, std::string> map(2);
    map.put(1, "one");
    map.put(2, "two");
    map.put(3, "three");

    CHECK(map.find(1) == nullptr);
    CHECK(map.find(2) != nullptr);
    CHECK(map.find(3) != nullptr);
    CHECK(map.size() == 2);
    CHECK(map.capacity() == 2);
}

TEST_CASE("lru_map::find marks an entry most recently used")
{
    osect::lru_map<int, std::string> map(2);
    map.put(1, "one");
    map.put(2, "two");
    map.find(1);
    map.put(3, "three");

    CHECK(map.find(1) != nullptr);
    CHECK(map.find(2) == nullptr);
}

TEST_CASE("lru_map::put replaces an existing value and marks it most recently used")
{
    osect::lru_map<int, std::string> map(2);
    map.put(1, "one");
    map.put(2, "two");
    map.put(1, "uno");
    map.put(3, "three");

    REQUIRE(map.find(1) != nullptr);
    CHECK(*map.find(1) == "uno");
    CHECK(map.find(2) == nullptr);
    CHECK(map.size() == 2);
}
