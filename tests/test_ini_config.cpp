#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "ini_config.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <string>
#include <unistd.h>

// RAII temp .ini file for each test case. Uses mkstemp for a race-free
// unique path, then writes the supplied contents via fstream.
namespace
{
    struct tmp_ini
    {
        std::string path;

        explicit tmp_ini(const std::string& contents)
        {
            char tmpl[] = "/tmp/nasrbrowse_test_XXXXXX";
            int fd = mkstemp(tmpl);
            REQUIRE(fd >= 0);
            close(fd);
            path = tmpl;

            std::ofstream f(path);
            f << contents;
        }

        ~tmp_ini() { std::remove(path.c_str()); }
    };
}

TEST_CASE("parse: section.key lookup returns the stored value")
{
    tmp_ini f("[map]\ncenter_lon = -98.0\ncenter_lat = 39.5\n");
    ini_config cfg(f.path);

    CHECK(cfg.exists("map.center_lon"));
    CHECK(cfg.exists("map.center_lat"));
    CHECK(cfg.get<std::string>("map.center_lon") == "-98.0");
    // std::stod and the compiler both produce the IEEE-correct closest
    // double for the source text, so the two sides are bit-identical.
    CHECK(cfg.get<double>("map.center_lat") == 39.5);
}

TEST_CASE("parse: whitespace around keys and values is trimmed")
{
    tmp_ini f("[section]\n  key1   =   value1  \nkey2=value2\n");
    ini_config cfg(f.path);

    CHECK(cfg.get<std::string>("section.key1") == "value1");
    CHECK(cfg.get<std::string>("section.key2") == "value2");
}

TEST_CASE("parse: ';' starts a line comment")
{
    tmp_ini f("[section]\n; this is a comment\nkey = value ; trailing\n");
    ini_config cfg(f.path);

    CHECK(cfg.get<std::string>("section.key") == "value");
}

TEST_CASE("parse: typed getters convert value")
{
    tmp_ini f("[nums]\ni = 42\nd = 3.14\nl = 10000000000\nu = 7\n");
    ini_config cfg(f.path);

    CHECK(cfg.get<int>("nums.i") == 42);
    CHECK(cfg.get<double>("nums.d") == 3.14);
    CHECK(cfg.get<long>("nums.l") == 10000000000L);
    CHECK(cfg.get<unsigned long>("nums.u") == 7UL);
}

TEST_CASE("exists: missing key returns false, get() on it throws")
{
    tmp_ini f("[section]\nkey = v\n");
    ini_config cfg(f.path);

    CHECK_FALSE(cfg.exists("section.missing"));
    CHECK_THROWS_AS(cfg.get<std::string>("section.missing"), std::runtime_error);
}

TEST_CASE("parse: multiple '=' in one line throws")
{
    tmp_ini f("[section]\nkey = a = b\n");
    CHECK_THROWS_AS(ini_config(f.path), std::runtime_error);
}

TEST_CASE("set: typed setters update the in-memory value")
{
    tmp_ini f("[section]\nkey = oldval\ni = 1\n");
    ini_config cfg(f.path);

    cfg.set<std::string>("section.key", "newval");
    CHECK(cfg.get<std::string>("section.key") == "newval");

    cfg.set<int>("section.i", 99);
    CHECK(cfg.get<int>("section.i") == 99);
}

TEST_CASE("set: observer is notified of changes")
{
    struct counting_observer : ini_config::observer
    {
        int calls = 0;
        std::string last_key, last_from, last_to;

        void observe_config_change(const std::string& key,
                                   const std::string& from,
                                   const std::string& to) override
        {
            ++calls;
            last_key = key;
            last_from = from;
            last_to = to;
        }
    };

    tmp_ini f("[section]\nkey = initial\n");
    ini_config cfg(f.path);

    counting_observer ob;
    cfg.observe("section.key", &ob);
    cfg.set<std::string>("section.key", "updated");

    CHECK(ob.calls == 1);
    CHECK(ob.last_key == "section.key");
    CHECK(ob.last_from == "initial");
    CHECK(ob.last_to == "updated");

    // After unobserve, no more notifications
    cfg.unobserve("section.key", &ob);
    cfg.set<std::string>("section.key", "third");
    CHECK(ob.calls == 1);
}

TEST_CASE("keys before any [section] header are ignored")
{
    tmp_ini f("orphan = value\n[section]\nkey = v\n");
    ini_config cfg(f.path);

    CHECK_FALSE(cfg.exists(".orphan"));
    CHECK_FALSE(cfg.exists("orphan"));
    CHECK(cfg.exists("section.key"));
}
