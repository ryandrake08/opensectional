#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "chart_style.hpp"
#include "ini_config.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

namespace
{
    struct tmp_ini
    {
        std::string path;

        explicit tmp_ini(const std::string& contents)
        {
            auto tmpl = (std::filesystem::temp_directory_path() /
                         "osect_chart_style_XXXXXX").string();
            int fd = mkstemp(tmpl.data());
            REQUIRE(fd >= 0);
            close(fd);
            path = tmpl;

            std::ofstream f(path);
            f << contents;
        }

        ~tmp_ini() { std::remove(path.c_str()); }
    };
}

TEST_CASE("defaults-only chart_style produces canonical VFR styles")
{
    ini_config empty;
    osect::chart_style cs(empty, osect::chart_mode::vfr);

    // tfr.vfr: min_zoom=6, color=crimson, line_width=2, dash=15, gap=8
    const auto& tfr = cs.tfr_style();
    CHECK(tfr.line_width == 2.0F);
    CHECK(tfr.dash_length == 15.0F);
    CHECK(tfr.gap_length == 8.0F);
    // crimson = #DC143C
    CHECK(tfr.r == doctest::Approx(0xDC / 255.0F));
    CHECK(tfr.g == doctest::Approx(0x14 / 255.0F));
    CHECK(tfr.b == doctest::Approx(0x3C / 255.0F));
    CHECK(cs.tfr_visible(6.0));
    CHECK_FALSE(cs.tfr_visible(5.0));

    // runway.vfr: min_zoom=10, color=silver, line_width=3, border_width=0
    const auto& rw = cs.runway_style();
    CHECK(rw.line_width == 3.0F);
    CHECK(rw.border_width == 0.0F);
    CHECK(cs.runway_visible(10.0));
    CHECK_FALSE(cs.runway_visible(9.0));

    // route.vfr: color=magenta, line_width=3 (border_width default 1)
    const auto& rt = cs.route_style();
    CHECK(rt.line_width == 3.0F);
    CHECK(rt.border_width == 1.0F);
    CHECK(rt.r == 1.0F);
    CHECK(rt.g == 0.0F);
    CHECK(rt.b == 1.0F);

    // airspace_b.vfr: line_width=2, color=royalblue
    const auto& ab = cs.airspace_style("B", "");
    CHECK(ab.line_width == 2.0F);
    CHECK(cs.airspace_visible("B", "", 5.0));
    CHECK_FALSE(cs.airspace_visible("B", "", 4.0));

    // V-airway: darkcyan, min_zoom=9
    CHECK(cs.airway_visible("V123", 9.0));
    CHECK_FALSE(cs.airway_visible("V123", 8.0));
}

TEST_CASE("ini override layers on top of defaults")
{
    tmp_ini f(
        "[tfr.vfr]\n"
        "line_width = 7.5\n"
        "[runway.vfr]\n"
        "color = red\n");
    ini_config ini(f.path);
    osect::chart_style cs(ini, osect::chart_mode::vfr);

    // Overridden keys take the new value
    CHECK(cs.tfr_style().line_width == 7.5F);
    const auto& rw = cs.runway_style();
    CHECK(rw.r == 1.0F);
    CHECK(rw.g == 0.0F);
    CHECK(rw.b == 0.0F);

    // Untouched fields keep their defaults
    CHECK(cs.tfr_style().dash_length == 15.0F);
    CHECK(cs.tfr_style().gap_length == 8.0F);
    CHECK(rw.line_width == 3.0F);
    CHECK(rw.border_width == 0.0F);
}

TEST_CASE("repo osect.ini reproduces the defaults-only style")
{
    // ctest sets WORKING_DIRECTORY to the repo root, so osect.ini
    // resolves directly. If this file ever drifts from the code
    // defaults, this test catches it.
    std::ifstream test("osect.ini");
    REQUIRE(test.good());

    ini_config defaults_ini;
    osect::chart_style defaults(defaults_ini, osect::chart_mode::vfr);

    ini_config from_file("osect.ini");
    osect::chart_style from_ini(from_file, osect::chart_mode::vfr);

    auto same = [](const osect::feature_style& a, const osect::feature_style& b)
    {
        return a.min_zoom == b.min_zoom
            && a.max_zoom == b.max_zoom
            && a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a
            && a.line_width == b.line_width
            && a.border_width == b.border_width
            && a.dash_length == b.dash_length
            && a.gap_length == b.gap_length;
    };

    CHECK(same(defaults.tfr_style(), from_ini.tfr_style()));
    CHECK(same(defaults.runway_style(), from_ini.runway_style()));
    CHECK(same(defaults.route_style(), from_ini.route_style()));
    CHECK(same(defaults.adiz_style(), from_ini.adiz_style()));
    CHECK(same(defaults.mtr_style(), from_ini.mtr_style()));
    CHECK(same(defaults.rco_style(), from_ini.rco_style()));
    CHECK(same(defaults.awos_style(), from_ini.awos_style()));
    CHECK(same(defaults.pja_area_style(), from_ini.pja_area_style()));
    CHECK(same(defaults.pja_point_style(), from_ini.pja_point_style()));
    CHECK(same(defaults.maa_area_style(), from_ini.maa_area_style()));
    CHECK(same(defaults.maa_point_style(), from_ini.maa_point_style()));

    CHECK(same(defaults.airspace_style("B", ""), from_ini.airspace_style("B", "")));
    CHECK(same(defaults.airspace_style("C", ""), from_ini.airspace_style("C", "")));
    CHECK(same(defaults.airspace_style("D", ""), from_ini.airspace_style("D", "")));
    CHECK(same(defaults.airspace_style("", "CLASS_E2"),
               from_ini.airspace_style("", "CLASS_E2")));

    CHECK(same(defaults.sua_style("PA"), from_ini.sua_style("PA")));
    CHECK(same(defaults.sua_style("RA"), from_ini.sua_style("RA")));
    CHECK(same(defaults.sua_style("WA"), from_ini.sua_style("WA")));
    CHECK(same(defaults.sua_style("AA"), from_ini.sua_style("AA")));
    CHECK(same(defaults.sua_style("MOA"), from_ini.sua_style("MOA")));
    CHECK(same(defaults.sua_style("NSA"), from_ini.sua_style("NSA")));

    CHECK(same(defaults.airway_style("V123"), from_ini.airway_style("V123")));
    CHECK(same(defaults.airway_style("J42"),  from_ini.airway_style("J42")));
    CHECK(same(defaults.airway_style("Q15"),  from_ini.airway_style("Q15")));
    CHECK(same(defaults.airway_style("RTE"),  from_ini.airway_style("RTE")));
    CHECK(same(defaults.airway_style("R2"),   from_ini.airway_style("R2")));

    CHECK(same(defaults.fix_style("WP"),  from_ini.fix_style("WP")));
    CHECK(same(defaults.fix_style("VFR"), from_ini.fix_style("VFR")));
    CHECK(same(defaults.fix_style("CN"),  from_ini.fix_style("CN")));
    CHECK(same(defaults.fix_style("MR"),  from_ini.fix_style("MR")));
    CHECK(same(defaults.fix_style("NRS"), from_ini.fix_style("NRS")));

    CHECK(same(defaults.navaid_style("VOR"),     from_ini.navaid_style("VOR")));
    CHECK(same(defaults.navaid_style("NDB"),     from_ini.navaid_style("NDB")));
    CHECK(same(defaults.navaid_style("NDB/DME"), from_ini.navaid_style("NDB/DME")));

    CHECK(same(defaults.obstacle_style(1500), from_ini.obstacle_style(1500)));
    CHECK(same(defaults.obstacle_style(500),  from_ini.obstacle_style(500)));
    CHECK(same(defaults.obstacle_style(50),   from_ini.obstacle_style(50)));
}
