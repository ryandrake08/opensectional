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

    fix make_fix(const std::string& charts, const std::string& use_code)
    {
        fix f;
        f.charts = charts;
        f.use_code = use_code;
        return f;
    }

    airport make_airport(int max_hard_rwy_len, bool hard_surface, bool has_iap_indicator,
                         const std::string& status = "O")
    {
        airport a;
        a.arpt_status = status;
        a.hard_surface = hard_surface;
        a.max_hard_rwy_len = max_hard_rwy_len;
        a.has_iap_indicator = has_iap_indicator;
        return a;
    }
} // namespace

// --- airport_on_chart --------------------------------------------------

TEST_CASE("airport_on_chart: every operational airport on Sectional")
{
    // Tiny private grass strip — still on Sectional.
    auto tiny = make_airport(/*rwy*/ 1300, /*hard*/ false, /*iap*/ false);
    CHECK(airport_on_chart(tiny, chart_type::sectional));

    // Large towered airport.
    auto big = make_airport(11301, true, true);
    CHECK(airport_on_chart(big, chart_type::sectional));
}

TEST_CASE("airport_on_chart: closed airports never drawn")
{
    auto closed = make_airport(5000, true, true, "CI");
    CHECK_FALSE(airport_on_chart(closed, chart_type::sectional));
    CHECK_FALSE(airport_on_chart(closed, chart_type::ifr_low));
    CHECK_FALSE(airport_on_chart(closed, chart_type::ifr_high));
}

TEST_CASE("airport_on_chart: IFR Low — IAP airport shows regardless of length")
{
    // IZA: 2803 ft hard surface but has NPI markings → on IFR Low.
    auto iza = make_airport(2803, true, true);
    CHECK(airport_on_chart(iza, chart_type::ifr_low));
    // Not on IFR High — runway too short.
    CHECK_FALSE(airport_on_chart(iza, chart_type::ifr_high));
}

TEST_CASE("airport_on_chart: IFR Low — non-IAP, hard surface, runway >= 3000")
{
    // 8CA8: PR, no IAP indicator, 3488 ft ASPH → on IFR Low.
    auto eight_ca8 = make_airport(3488, true, false);
    CHECK(airport_on_chart(eight_ca8, chart_type::ifr_low));
    CHECK_FALSE(airport_on_chart(eight_ca8, chart_type::ifr_high));

    // F72: 3123 ft hard surface, no IAP → on IFR Low.
    auto f72 = make_airport(3123, true, false);
    CHECK(airport_on_chart(f72, chart_type::ifr_low));
}

TEST_CASE("airport_on_chart: IFR Low — non-IAP, hard surface, runway < 3000 hidden")
{
    // O33/O16/1O6: ~2700-2800 ft hard, no IAP → not on IFR Low.
    for(auto rwy : {2700, 2783, 2800})
    {
        auto a = make_airport(rwy, true, false);
        CHECK_FALSE(airport_on_chart(a, chart_type::ifr_low));
        CHECK(airport_on_chart(a, chart_type::sectional));
    }
}

TEST_CASE("airport_on_chart: IFR Low — soft surface never qualifies via length rule")
{
    // 29CN/CN38: dirt/turf strips. Neither IAP nor hard → not on IFR Low.
    auto soft = make_airport(/*rwy*/ 0, /*hard*/ false, /*iap*/ false);
    CHECK_FALSE(airport_on_chart(soft, chart_type::ifr_low));
    CHECK_FALSE(airport_on_chart(soft, chart_type::ifr_high));
    CHECK(airport_on_chart(soft, chart_type::sectional));
}

TEST_CASE("airport_on_chart: IFR High — runway >= 5000 ft hard surface")
{
    // VGT: 5005 ft (just over) → IFR High.
    auto vgt = make_airport(5005, true, true);
    CHECK(airport_on_chart(vgt, chart_type::ifr_high));

    // VCB: 4700 ft hard, has IAP → IFR Low yes, IFR High no.
    auto vcb = make_airport(4700, true, true);
    CHECK(airport_on_chart(vcb, chart_type::ifr_low));
    CHECK_FALSE(airport_on_chart(vcb, chart_type::ifr_high));

    // NV83: 6776 ft hard, no IAP → IFR Low + High.
    auto nv83 = make_airport(6776, true, false);
    CHECK(airport_on_chart(nv83, chart_type::ifr_low));
    CHECK(airport_on_chart(nv83, chart_type::ifr_high));
}

TEST_CASE("airport_on_chart: IFR High — IAP doesn't override the 5000 ft floor")
{
    // IAP airport with 4500 ft hard runway: on IFR Low (IAP override),
    // not on IFR High (length rules).
    auto a = make_airport(4500, true, true);
    CHECK(airport_on_chart(a, chart_type::ifr_low));
    CHECK_FALSE(airport_on_chart(a, chart_type::ifr_high));
}

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

TEST_CASE("navaid_on_chart: standalone TACAN not on Sectional")
{
    // NQX/NKX/CSG verified absent from sectionals — standalone TACAN is
    // military-only. IFR Low still depicts them; IFR High depends on
    // the promotion flag.
    auto tac = make_navaid("TACAN", "");
    CHECK_FALSE(navaid_on_chart(tac, chart_type::sectional));
    CHECK(navaid_on_chart(tac, chart_type::ifr_low));
    CHECK_FALSE(navaid_on_chart(tac, chart_type::ifr_high));
}

TEST_CASE("navaid_on_chart: TACAN with promotion flag — IFR Low + IFR High, still not Sectional")
{
    auto tac = make_navaid("TACAN", "", "Y");
    CHECK_FALSE(navaid_on_chart(tac, chart_type::sectional));
    CHECK(navaid_on_chart(tac, chart_type::ifr_low));
    CHECK(navaid_on_chart(tac, chart_type::ifr_high));
}

TEST_CASE("navaid_on_chart: VORTAC stays on Sectional (collocated VOR+TACAN)")
{
    // VORTAC is the civilian VOR collocated with a military TACAN — a
    // distinct NAV_TYPE. Always charted on Sectional + IFR Low; on IFR
    // High when ALT_CODE='H'/'VH' or LOW_NAV_ON_HIGH_CHART_FLAG='Y'.
    auto vt = make_navaid("VORTAC", "L");
    CHECK(navaid_on_chart(vt, chart_type::sectional));
    CHECK(navaid_on_chart(vt, chart_type::ifr_low));
    CHECK_FALSE(navaid_on_chart(vt, chart_type::ifr_high));
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
    // STAR/SID-only fixes (no en-route role) are not depicted on
    // Sectionals — sectional inclusion via STAR/SID requires
    // ENROUTE LOW co-membership.
    for(const auto* charts : {"SID", "STAR", "SPECIAL IAP", "MILITARY IAP", "NOT REQUIRED"})
    {
        auto f = make_fix(charts);
        CHECK_FALSE(fix_on_chart(f, chart_type::sectional));
        CHECK_FALSE(fix_on_chart(f, chart_type::ifr_low));
        CHECK_FALSE(fix_on_chart(f, chart_type::ifr_high));
    }
}

// --- fix_on_chart: WP-on-en-route, STAR/SID-on-en-route, RP-with-CONTROLLER

TEST_CASE("fix_on_chart: WP on ENROUTE LOW shows on Sectional + IFR Low")
{
    // SMONE: WP, CONTROLLER LOW + ENROUTE LOW + IAP — verified to show
    // on Sectional even though it has IAP, because WP+ENROUTE LOW is
    // sufficient.
    auto smone = make_fix("CONTROLLER LOW,ENROUTE LOW,IAP", "WP");
    CHECK(fix_on_chart(smone, chart_type::sectional));
    CHECK(fix_on_chart(smone, chart_type::ifr_low));
    CHECK_FALSE(fix_on_chart(smone, chart_type::ifr_high));
}

TEST_CASE("fix_on_chart: WP on STAR + ENROUTE LOW shows on Sectional")
{
    // NORCL: WP, CONTROLLER LOW + ENROUTE LOW + STAR
    auto norcl = make_fix("CONTROLLER LOW,ENROUTE LOW,STAR", "WP");
    CHECK(fix_on_chart(norcl, chart_type::sectional));
    CHECK(fix_on_chart(norcl, chart_type::ifr_low));
    CHECK_FALSE(fix_on_chart(norcl, chart_type::ifr_high));

    // MOVDD: WP, CONTROLLER + ENROUTE LOW + IAP + STAR
    auto movdd = make_fix("CONTROLLER,ENROUTE LOW,IAP,STAR", "WP");
    CHECK(fix_on_chart(movdd, chart_type::sectional));
    CHECK(fix_on_chart(movdd, chart_type::ifr_low));
    CHECK_FALSE(fix_on_chart(movdd, chart_type::ifr_high));
}

TEST_CASE("fix_on_chart: RP with STAR or SID on ENROUTE LOW shows on Sectional")
{
    // BORED: RP, CONTROLLER LOW + ENROUTE LOW + IAP + STAR
    auto bored = make_fix("CONTROLLER LOW,ENROUTE LOW,IAP,STAR", "RP");
    CHECK(fix_on_chart(bored, chart_type::sectional));
    CHECK(fix_on_chart(bored, chart_type::ifr_low));

    // KARNN: RP, CONTROLLER LOW + ENROUTE LOW + STAR (no IAP)
    auto karnn = make_fix("CONTROLLER LOW,ENROUTE LOW,STAR", "RP");
    CHECK(fix_on_chart(karnn, chart_type::sectional));
    CHECK(fix_on_chart(karnn, chart_type::ifr_low));

    // HENCE: RP, CONTROLLER LOW + ENROUTE LOW + SID
    auto hence = make_fix("CONTROLLER LOW,ENROUTE LOW,SID", "RP");
    CHECK(fix_on_chart(hence, chart_type::sectional));
    CHECK(fix_on_chart(hence, chart_type::ifr_low));
}

TEST_CASE("fix_on_chart: RP with un-suffixed CONTROLLER + ENROUTE LOW (no IAP) shows on Sectional")
{
    // WINDY: RP, CONTROLLER + ENROUTE LOW only — TRACON jurisdiction,
    // no IAP. Shows on Sectional.
    auto windy = make_fix("CONTROLLER,ENROUTE LOW", "RP");
    CHECK(fix_on_chart(windy, chart_type::sectional));
    CHECK(fix_on_chart(windy, chart_type::ifr_low));
    CHECK_FALSE(fix_on_chart(windy, chart_type::ifr_high));
}

TEST_CASE("fix_on_chart: RP with CONTROLLER + ENROUTE LOW + IAP — IFR Low only")
{
    // HONEZ / AWALI: RP, CONTROLLER + ENROUTE LOW + IAP. IAP marks the
    // fix as a terminal-procedure construct that belongs on the IAP
    // plate, not the Sectional, even though it's in TRACON jurisdiction.
    for(const auto* id : {"HONEZ", "AWALI"})
    {
        (void)id;
        auto f = make_fix("CONTROLLER,ENROUTE LOW,IAP", "RP");
        CHECK_FALSE(fix_on_chart(f, chart_type::sectional));
        CHECK(fix_on_chart(f, chart_type::ifr_low));
        CHECK_FALSE(fix_on_chart(f, chart_type::ifr_high));
    }
}

TEST_CASE("fix_on_chart: RP with CONTROLLER LOW (ARTCC) + ENROUTE LOW — IFR Low only")
{
    // SNUPY/HUNTE/OLRIO/FIDDO/WHEEL/PANOS: RP, CONTROLLER LOW + ENROUTE
    // LOW. CONTROLLER LOW means ARTCC sector, not TRACON — these are
    // pure en-route reporting points in ARTCC jurisdiction and are not
    // depicted on Sectionals.
    auto snupy = make_fix("CONTROLLER LOW,ENROUTE LOW", "RP");
    CHECK_FALSE(fix_on_chart(snupy, chart_type::sectional));
    CHECK(fix_on_chart(snupy, chart_type::ifr_low));
    CHECK_FALSE(fix_on_chart(snupy, chart_type::ifr_high));
}

TEST_CASE("fix_on_chart: RP with CONTROLLER LOW + ENROUTE LOW + IAP — IFR Low only")
{
    // MUREQ / KATSO: RP, CONTROLLER LOW + ENROUTE LOW + IAP. Same as
    // SNUPY but with IAP. Still IFR Low only.
    auto mureq = make_fix("CONTROLLER LOW,ENROUTE LOW,IAP", "RP");
    CHECK_FALSE(fix_on_chart(mureq, chart_type::sectional));
    CHECK(fix_on_chart(mureq, chart_type::ifr_low));
    CHECK_FALSE(fix_on_chart(mureq, chart_type::ifr_high));
}

TEST_CASE("fix_on_chart: STAR-only fix (no ENROUTE LOW) doesn't show on Sectional")
{
    // STAR-only WPs are intermediate STAR plate fixes — they appear on
    // the procedure plate but not on the Sectional / Enroute charts.
    auto aaame = make_fix("STAR", "WP");
    CHECK_FALSE(fix_on_chart(aaame, chart_type::sectional));
    CHECK_FALSE(fix_on_chart(aaame, chart_type::ifr_low));
    CHECK_FALSE(fix_on_chart(aaame, chart_type::ifr_high));
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
