#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "ephemeral_cache.hpp"

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

using osect::ephemeral_cache;

namespace
{
    // RAII tmp-dir helper. doctest cases each get their own
    // sandboxed cache so leftover files from one test don't bleed
    // into another.
    struct tmp_dir
    {
        std::filesystem::path path;
        explicit tmp_dir(const char* tag)
        {
            const auto base = std::filesystem::temp_directory_path() /
                ("osect_cache_test_" + std::string(tag) + "_XXXXXX");
            // mktemp-style: append a counter that's unique per test.
            // Plenty of races possible in theory, fine in practice.
            for(int i = 0; i < 1000; ++i)
            {
                const auto candidate = base.string() +
                    std::to_string(i);
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
}

TEST_CASE("ephemeral_cache: store and load round-trip")
{
    tmp_dir dir("roundtrip");
    ephemeral_cache cache(dir.path);

    cache.store("tfr", "hello world", "\"abc123\"");
    auto loaded = cache.load("tfr");
    REQUIRE(loaded.has_value());
    CHECK(loaded->body == "hello world");
    CHECK(loaded->etag == "\"abc123\"");
}

TEST_CASE("ephemeral_cache: empty body and empty etag round-trip")
{
    tmp_dir dir("empty");
    ephemeral_cache cache(dir.path);

    cache.store("source1", "", "");
    auto loaded = cache.load("source1");
    REQUIRE(loaded.has_value());
    CHECK(loaded->body.empty());
    CHECK(loaded->etag.empty());
}

TEST_CASE("ephemeral_cache: missing file returns nullopt")
{
    tmp_dir dir("missing");
    ephemeral_cache cache(dir.path);
    CHECK(!cache.load("never-stored").has_value());
}

TEST_CASE("ephemeral_cache: truncated file returns nullopt")
{
    tmp_dir dir("truncated");
    ephemeral_cache cache(dir.path);
    cache.store("tfr", "some payload bytes", "\"etag\"");

    // Truncate the cache file to half-length to simulate partial
    // write or filesystem corruption.
    const auto file = dir.path / "tfr.bin";
    const auto sz = std::filesystem::file_size(file);
    std::filesystem::resize_file(file, sz / 2);

    CHECK(!cache.load("tfr").has_value());
}

TEST_CASE("ephemeral_cache: bad magic returns nullopt")
{
    tmp_dir dir("badmagic");
    ephemeral_cache cache(dir.path);
    // Write garbage where a cache file would be.
    const auto file = dir.path / "tfr.bin";
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << "this is not a cache file";
    }
    CHECK(!cache.load("tfr").has_value());
}

TEST_CASE("ephemeral_cache: store replaces prior entry")
{
    tmp_dir dir("replace");
    ephemeral_cache cache(dir.path);

    cache.store("tfr", "first body", "\"v1\"");
    cache.store("tfr", "second body, longer than first", "\"v2\"");

    auto loaded = cache.load("tfr");
    REQUIRE(loaded.has_value());
    CHECK(loaded->body == "second body, longer than first");
    CHECK(loaded->etag == "\"v2\"");

    // No leftover .tmp file from the atomic-replace.
    CHECK(!std::filesystem::exists(dir.path / "tfr.bin.tmp"));
}

TEST_CASE("ephemeral_cache: clear removes existing file, no-op for missing")
{
    tmp_dir dir("clear");
    ephemeral_cache cache(dir.path);

    cache.store("tfr", "body", "\"v\"");
    CHECK(cache.load("tfr").has_value());
    cache.clear("tfr");
    CHECK(!cache.load("tfr").has_value());
    // Second clear on a missing entry should not throw.
    cache.clear("tfr");
}

TEST_CASE("ephemeral_cache: rejects path-traversal and invalid names")
{
    tmp_dir dir("badname");
    ephemeral_cache cache(dir.path);

    CHECK_THROWS_AS(cache.load("../escape"), std::runtime_error);
    CHECK_THROWS_AS(cache.load("foo/bar"), std::runtime_error);
    CHECK_THROWS_AS(cache.store("foo bar", "x", ""), std::runtime_error);
    CHECK_THROWS_AS(cache.load(""), std::runtime_error);
}

TEST_CASE("ephemeral_cache: large body round-trips intact")
{
    tmp_dir dir("large");
    ephemeral_cache cache(dir.path);
    // Bigger than any single buffered read; checks that the loader
    // pulls the full payload, not just the first chunk.
    std::string body(2 * 1024 * 1024, 'a');
    body[1024 * 1024] = 'X';  // mid-buffer sentinel
    cache.store("large", body, "");
    auto loaded = cache.load("large");
    REQUIRE(loaded.has_value());
    CHECK(loaded->body.size() == body.size());
    CHECK(loaded->body[1024 * 1024] == 'X');
    CHECK(loaded->body == body);
}
