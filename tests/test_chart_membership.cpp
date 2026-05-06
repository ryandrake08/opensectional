#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "chart_type.hpp"
#include "nasr_database.hpp"

using namespace osect;

namespace
{
    // Construct a navaid populated with just the fields the chart-membership
    // rule consults. All other fields default to empty strings / zero so the
    // test reads cleanly.
    navaid make_navaid(const std::string& nav_type, const std::string& alt_code,
                       const std::string& low_on_high = "")
    {
        navaid n;
        n.nav_type = nav_type;
        n.alt_code = alt_code;
        n.low_nav_on_high_chart_flag = low_on_high;
        return n;
    }

    fix make_fix(const std::string& charts)
    {
        fix f;
        f.charts = charts;
        return f;
    }
} // namespace

// --- navaid_on_chart ---------------------------------------------------

TEST_CASE("navaid_on_chart: ALT_CODE=H VORs (e.g. AKN/DLG/ODK) on all three charts")
{
    auto akn = make_navaid("VORTAC", "H");
    CHECK(navaid_on_chart(akn, chart_type::sectional));
    CHECK(navaid_on_chart(akn, chart_type::ifr_low));
    CHECK(navaid_on_chart(akn, chart_type::ifr_high));

    auto dlg = make_navaid("VOR/DME", "H");
    CHECK(navaid_on_chart(dlg, chart_type::sectional));
    CHECK(navaid_on_chart(dlg, chart_type::ifr_low));
    CHECK(navaid_on_chart(dlg, chart_type::ifr_high));
}

TEST_CASE("navaid_on_chart: ALT_CODE=L VOR (e.g. MOS) on Sectional + IFR Low only")
{
    auto mos = make_navaid("VOR/DME", "L");
    CHECK(navaid_on_chart(mos, chart_type::sectional));
    CHECK(navaid_on_chart(mos, chart_type::ifr_low));
    CHECK_FALSE(navaid_on_chart(mos, chart_type::ifr_high));
}

TEST_CASE("navaid_on_chart: ALT_CODE=VH VOR on all three charts (high + low band)")
{
    auto vh = make_navaid("VORTAC", "VH");
    CHECK(navaid_on_chart(vh, chart_type::sectional));
    CHECK(navaid_on_chart(vh, chart_type::ifr_low));
    CHECK(navaid_on_chart(vh, chart_type::ifr_high));
}

TEST_CASE("navaid_on_chart: LOW_NAV_ON_HIGH_CHART_FLAG=Y promotes onto IFR High")
{
    auto promoted = make_navaid("VOR/DME", "L", "Y");
    CHECK(navaid_on_chart(promoted, chart_type::sectional));
    CHECK(navaid_on_chart(promoted, chart_type::ifr_low));
    CHECK(navaid_on_chart(promoted, chart_type::ifr_high));
}

TEST_CASE("navaid_on_chart: NDB on Sectional + IFR Low (no ALT_CODE)")
{
    auto ndb = make_navaid("NDB", "");
    CHECK(navaid_on_chart(ndb, chart_type::sectional));
    CHECK(navaid_on_chart(ndb, chart_type::ifr_low));
    CHECK_FALSE(navaid_on_chart(ndb, chart_type::ifr_high));
}

TEST_CASE("navaid_on_chart: TACAN with promotion flag on all three")
{
    auto tac = make_navaid("TACAN", "", "Y");
    CHECK(navaid_on_chart(tac, chart_type::sectional));
    CHECK(navaid_on_chart(tac, chart_type::ifr_low));
    CHECK(navaid_on_chart(tac, chart_type::ifr_high));
}

TEST_CASE("navaid_on_chart: VOT / FAN MARKER / MARINE NDB are never charted")
{
    for(const auto* type : {"VOT", "FAN MARKER", "MARINE NDB"})
    {
        auto n = make_navaid(type, "");
        CHECK_FALSE(navaid_on_chart(n, chart_type::sectional));
        CHECK_FALSE(navaid_on_chart(n, chart_type::ifr_low));
        CHECK_FALSE(navaid_on_chart(n, chart_type::ifr_high));
    }
}

// --- fix_on_chart ------------------------------------------------------

TEST_CASE("fix_on_chart: ENROUTE HIGH only (e.g. SOSBY/JERKI) — IFR High only")
{
    auto sosby = make_fix("CONTROLLER HIGH,ENROUTE HIGH");
    CHECK_FALSE(fix_on_chart(sosby, chart_type::sectional));
    CHECK_FALSE(fix_on_chart(sosby, chart_type::ifr_low));
    CHECK(fix_on_chart(sosby, chart_type::ifr_high));

    auto jerki = make_fix("ENROUTE HIGH");
    CHECK_FALSE(fix_on_chart(jerki, chart_type::sectional));
    CHECK_FALSE(fix_on_chart(jerki, chart_type::ifr_low));
    CHECK(fix_on_chart(jerki, chart_type::ifr_high));
}

TEST_CASE("fix_on_chart: ENROUTE HIGH + STAR — IFR High only")
{
    auto cuttz = make_fix("CONTROLLER HIGH,ENROUTE HIGH,STAR");
    CHECK_FALSE(fix_on_chart(cuttz, chart_type::sectional));
    CHECK_FALSE(fix_on_chart(cuttz, chart_type::ifr_low));
    CHECK(fix_on_chart(cuttz, chart_type::ifr_high));
}

TEST_CASE("fix_on_chart: both ENROUTE LOW and ENROUTE HIGH — both IFR products")
{
    auto fapis = make_fix("ENROUTE HIGH,ENROUTE LOW");
    CHECK_FALSE(fix_on_chart(fapis, chart_type::sectional));
    CHECK(fix_on_chart(fapis, chart_type::ifr_low));
    CHECK(fix_on_chart(fapis, chart_type::ifr_high));
}

TEST_CASE("fix_on_chart: ENROUTE LOW only (e.g. RIPON) — IFR Low only")
{
    auto ripon = make_fix("ENROUTE LOW");
    CHECK_FALSE(fix_on_chart(ripon, chart_type::sectional));
    CHECK(fix_on_chart(ripon, chart_type::ifr_low));
    CHECK_FALSE(fix_on_chart(ripon, chart_type::ifr_high));
}

TEST_CASE("fix_on_chart: SECTIONAL token (e.g. VPESS) — Sectional only")
{
    auto vpess = make_fix("SECTIONAL");
    CHECK(fix_on_chart(vpess, chart_type::sectional));
    CHECK_FALSE(fix_on_chart(vpess, chart_type::ifr_low));
    CHECK_FALSE(fix_on_chart(vpess, chart_type::ifr_high));
}

TEST_CASE("fix_on_chart: VFR FLYWAY PLANNING / VFR TERMINAL AREA — Sectional only")
{
    auto vplpp = make_fix("VFR FLYWAY PLANNING,VFR TERMINAL AREA");
    CHECK(fix_on_chart(vplpp, chart_type::sectional));
    CHECK_FALSE(fix_on_chart(vplpp, chart_type::ifr_low));
    CHECK_FALSE(fix_on_chart(vplpp, chart_type::ifr_high));
}

TEST_CASE("fix_on_chart: IAP-only fix (e.g. OPULY) draws on no chart product")
{
    auto opuly = make_fix("IAP");
    CHECK_FALSE(fix_on_chart(opuly, chart_type::sectional));
    CHECK_FALSE(fix_on_chart(opuly, chart_type::ifr_low));
    CHECK_FALSE(fix_on_chart(opuly, chart_type::ifr_high));
}

TEST_CASE("fix_on_chart: SID / STAR / SPECIAL IAP-only fixes draw nowhere")
{
    for(const auto* charts : {"SID", "STAR", "SPECIAL IAP", "MILITARY IAP", "NOT REQUIRED"})
    {
        auto f = make_fix(charts);
        CHECK_FALSE(fix_on_chart(f, chart_type::sectional));
        CHECK_FALSE(fix_on_chart(f, chart_type::ifr_low));
        CHECK_FALSE(fix_on_chart(f, chart_type::ifr_high));
    }
}

TEST_CASE("fix_on_chart: token match must be exact, not substring")
{
    // CONTROLLER LOW must not satisfy a search for ENROUTE LOW.
    auto controller_only = make_fix("CONTROLLER LOW");
    CHECK_FALSE(fix_on_chart(controller_only, chart_type::ifr_low));

    // CONTROLLER HIGH must not satisfy ENROUTE HIGH.
    auto ch_only = make_fix("CONTROLLER HIGH");
    CHECK_FALSE(fix_on_chart(ch_only, chart_type::ifr_high));
}

// --- airway_on_chart ---------------------------------------------------

TEST_CASE("airway_on_chart: V (Victor) on Sectional + IFR Low")
{
    CHECK(airway_on_chart("V123", chart_type::sectional));
    CHECK(airway_on_chart("V123", chart_type::ifr_low));
    CHECK_FALSE(airway_on_chart("V123", chart_type::ifr_high));
}

TEST_CASE("airway_on_chart: T (T-route GPS) on Sectional + IFR Low")
{
    CHECK(airway_on_chart("T270", chart_type::sectional));
    CHECK(airway_on_chart("T270", chart_type::ifr_low));
    CHECK_FALSE(airway_on_chart("T270", chart_type::ifr_high));
}

TEST_CASE("airway_on_chart: J (Jet) on IFR High only")
{
    CHECK_FALSE(airway_on_chart("J80", chart_type::sectional));
    CHECK_FALSE(airway_on_chart("J80", chart_type::ifr_low));
    CHECK(airway_on_chart("J80", chart_type::ifr_high));
}

TEST_CASE("airway_on_chart: Q / Y / H (RNAV high) on IFR High only")
{
    for(const auto* id : {"Q100", "Y15", "H1"})
    {
        CHECK_FALSE(airway_on_chart(id, chart_type::sectional));
        CHECK_FALSE(airway_on_chart(id, chart_type::ifr_low));
        CHECK(airway_on_chart(id, chart_type::ifr_high));
    }
}

TEST_CASE("airway_on_chart: TK (LF/MF) on IFR Low only")
{
    CHECK_FALSE(airway_on_chart("TK1", chart_type::sectional));
    CHECK(airway_on_chart("TK1", chart_type::ifr_low));
    CHECK_FALSE(airway_on_chart("TK1", chart_type::ifr_high));
}

TEST_CASE("airway_on_chart: BR / AR / RTE on both IFR products, not Sectional")
{
    for(const auto* id : {"BR5", "AR1", "RTE1"})
    {
        CHECK_FALSE(airway_on_chart(id, chart_type::sectional));
        CHECK(airway_on_chart(id, chart_type::ifr_low));
        CHECK(airway_on_chart(id, chart_type::ifr_high));
    }
}

// --- obstacle_on_chart -------------------------------------------------

TEST_CASE("obstacle_on_chart: Sectional only")
{
    CHECK(obstacle_on_chart(chart_type::sectional));
    CHECK_FALSE(obstacle_on_chart(chart_type::ifr_low));
    CHECK_FALSE(obstacle_on_chart(chart_type::ifr_high));
}

// --- mtr_on_chart ------------------------------------------------------

TEST_CASE("mtr_on_chart: VR routes on Sectional only")
{
    CHECK(mtr_on_chart("VR", chart_type::sectional));
    CHECK_FALSE(mtr_on_chart("VR", chart_type::ifr_low));
    CHECK_FALSE(mtr_on_chart("VR", chart_type::ifr_high));
}

TEST_CASE("mtr_on_chart: IR routes on Sectional + IFR Low")
{
    CHECK(mtr_on_chart("IR", chart_type::sectional));
    CHECK(mtr_on_chart("IR", chart_type::ifr_low));
    CHECK_FALSE(mtr_on_chart("IR", chart_type::ifr_high));
}
