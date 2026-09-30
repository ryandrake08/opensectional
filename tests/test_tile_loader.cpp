#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "tile_loader.hpp"

#include <atomic>
#include <chrono>
#include <future>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace
{
    using std::chrono::milliseconds;

    // Counts load calls per key and can hold the worker inside one key's
    // load until release() is called.
    struct test_load
    {
        std::mutex mutex;
        std::map<int, int> calls; // by key.x
        std::promise<void> gate;
        std::shared_future<void> released = gate.get_future().share();
        int blocking_x = -1;
        int failing_x = -1;

        int operator()(const osect::tile_key& key)
        {
            {
                std::scoped_lock lock(mutex);
                calls[key.x]++;
            }
            if(key.x == blocking_x)
            {
                released.wait();
            }
            if(key.x == failing_x)
            {
                throw std::runtime_error("no tile");
            }
            return key.x * 10;
        }

        int calls_for(int x)
        {
            std::scoped_lock lock(mutex);
            return calls[x];
        }

        void release()
        {
            gate.set_value();
        }
    };

    // Drains until count results have arrived or a second has passed.
    std::vector<int> drain_until(osect::tile_loader<int>& loader, std::size_t count)
    {
        std::vector<int> results;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while(results.size() < count && std::chrono::steady_clock::now() < deadline)
        {
            for(int r : loader.drain())
            {
                results.push_back(r);
            }
            std::this_thread::sleep_for(milliseconds(1));
        }
        return results;
    }

    bool wait_for_failure(osect::tile_loader<int>& loader, const osect::tile_key& key)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while(!loader.failed(key) && std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(milliseconds(1));
        }
        return loader.failed(key);
    }
}

TEST_CASE("tile_loader delivers a loaded tile through drain and signals readiness")
{
    test_load load;
    std::atomic<int> ready{0};
    osect::tile_loader<int> loader(std::ref(load), [&ready] { ready++; });

    loader.request({4, 3, 1});

    CHECK(drain_until(loader, 1) == std::vector<int>{30});
    CHECK(ready >= 1);
}

TEST_CASE("tile_loader marks a failed key and never loads it again")
{
    test_load load;
    load.failing_x = 2;
    osect::tile_loader<int> loader(std::ref(load), [] {});

    loader.request({4, 2, 0});
    REQUIRE(wait_for_failure(loader, {4, 2, 0}));

    loader.request({4, 2, 0});
    loader.request({4, 5, 0});
    CHECK(drain_until(loader, 1) == std::vector<int>{50}); // the worker is FIFO, so the retry would have run first
    CHECK(load.calls_for(2) == 1);
}

TEST_CASE("tile_loader ignores a request for a key already being loaded")
{
    test_load load;
    load.blocking_x = 1;
    osect::tile_loader<int> loader(std::ref(load), [] {});

    loader.request({4, 1, 0});
    loader.request({4, 1, 0});
    load.release();

    CHECK(drain_until(loader, 1) == std::vector<int>{10});
    loader.request({4, 6, 0});
    CHECK(drain_until(loader, 1) == std::vector<int>{60});
    CHECK(load.calls_for(1) == 1);
}

TEST_CASE("tile_loader cancel drops requests that have not started")
{
    test_load load;
    load.blocking_x = 1;
    osect::tile_loader<int> loader(std::ref(load), [] {});

    loader.request({4, 1, 0});
    // Wait until the worker is inside the blocking load, so key 2 stays queued.
    while(load.calls_for(1) == 0)
    {
        std::this_thread::sleep_for(milliseconds(1));
    }
    loader.request({4, 2, 0});
    loader.cancel();
    load.release();

    CHECK(drain_until(loader, 1) == std::vector<int>{10});
    loader.request({4, 7, 0});
    CHECK(drain_until(loader, 1) == std::vector<int>{70});
    CHECK(load.calls_for(2) == 0);
}
