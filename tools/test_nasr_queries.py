#!/usr/bin/env python3
"""Test NASR database queries for correctness and performance."""

import sqlite3
import sys
import time


# --- Spatial query functions (used by both correctness and performance tests) ---

def query_airports(conn, lon_min, lat_min, lon_max, lat_max):
    return conn.execute("""
        SELECT ARPT_ID, ARPT_NAME, LAT_DECIMAL, LONG_DECIMAL
        FROM APT_BASE
        WHERE rowid IN (
            SELECT id FROM APT_BASE_RTREE
            WHERE max_lon >= ? AND min_lon <= ?
              AND max_lat >= ? AND min_lat <= ?
        )
    """, (lon_min, lon_max, lat_min, lat_max)).fetchall()


def query_navaids(conn, lon_min, lat_min, lon_max, lat_max):
    return conn.execute("""
        SELECT NAV_ID, NAV_TYPE, NAME, LAT_DECIMAL, LONG_DECIMAL
        FROM NAV_BASE
        WHERE rowid IN (
            SELECT id FROM NAV_BASE_RTREE
            WHERE max_lon >= ? AND min_lon <= ?
              AND max_lat >= ? AND min_lat <= ?
        )
    """, (lon_min, lon_max, lat_min, lat_max)).fetchall()


def query_fixes(conn, lon_min, lat_min, lon_max, lat_max):
    return conn.execute("""
        SELECT FIX_ID, LAT_DECIMAL, LONG_DECIMAL
        FROM FIX_BASE
        WHERE rowid IN (
            SELECT id FROM FIX_BASE_RTREE
            WHERE max_lon >= ? AND min_lon <= ?
              AND max_lat >= ? AND min_lat <= ?
        )
    """, (lon_min, lon_max, lat_min, lat_max)).fetchall()


def query_airways(conn, lon_min, lat_min, lon_max, lat_max):
    return conn.execute("""
        SELECT AWY_ID, FROM_POINT, TO_POINT, FROM_LAT, FROM_LON, TO_LAT, TO_LON
        FROM AWY_SEG
        WHERE rowid IN (
            SELECT id FROM AWY_SEG_RTREE
            WHERE max_lon >= ? AND min_lon <= ?
              AND max_lat >= ? AND min_lat <= ?
        )
    """, (lon_min, lon_max, lat_min, lat_max)).fetchall()


def query_maa(conn, lon_min, lat_min, lon_max, lat_max):
    return conn.execute("""
        SELECT MAA_ID, TYPE, NAME
        FROM MAA_BASE
        WHERE rowid IN (
            SELECT id FROM MAA_BASE_RTREE
            WHERE max_lon >= ? AND min_lon <= ?
              AND max_lat >= ? AND min_lat <= ?
        )
    """, (lon_min, lon_max, lat_min, lat_max)).fetchall()


def query_class_airspace(conn, lon_min, lat_min, lon_max, lat_max):
    return conn.execute("""
        SELECT ARSP_ID, NAME, CLASS, LOCAL_TYPE, LOWER_VAL, UPPER_VAL
        FROM CLS_ARSP_BASE
        WHERE ARSP_ID IN (
            SELECT id FROM CLS_ARSP_BASE_RTREE
            WHERE max_lon >= ? AND min_lon <= ?
              AND max_lat >= ? AND min_lat <= ?
        )
    """, (lon_min, lon_max, lat_min, lat_max)).fetchall()


def query_sua(conn, lon_min, lat_min, lon_max, lat_max):
    return conn.execute("""
        SELECT SUA_ID, DESIGNATOR, NAME, SUA_TYPE, UPPER_LIMIT, LOWER_LIMIT
        FROM SUA_BASE
        WHERE SUA_ID IN (
            SELECT id FROM SUA_BASE_RTREE
            WHERE max_lon >= ? AND min_lon <= ?
              AND max_lat >= ? AND min_lat <= ?
        )
    """, (lon_min, lon_max, lat_min, lat_max)).fetchall()


def query_obstacles(conn, lon_min, lat_min, lon_max, lat_max):
    return conn.execute("""
        SELECT OAS_NUM, OBSTACLE_TYPE, AGL_HT, AMSL_HT, STATE, CITY
        FROM OBS_BASE
        WHERE rowid IN (
            SELECT id FROM OBS_BASE_RTREE
            WHERE max_lon >= ? AND min_lon <= ?
              AND max_lat >= ? AND min_lat <= ?
        )
    """, (lon_min, lon_max, lat_min, lat_max)).fetchall()


def query_class_airspace_with_shapes(conn, lon_min, lat_min, lon_max, lat_max):
    """Query class airspace and load shape points (full query path)."""
    bases = query_class_airspace(conn, lon_min, lat_min, lon_max, lat_max)
    total_points = 0
    for row in bases:
        points = conn.execute(
            "SELECT LON_DECIMAL, LAT_DECIMAL FROM CLS_ARSP_SHP "
            "WHERE ARSP_ID = ? ORDER BY PART_NUM, POINT_SEQ",
            (row[0],),
        ).fetchall()
        total_points += len(points)
    return bases, total_points


# --- Test assertion helpers ---

def benchmark(label, func, *args, iterations=100):
    result = func(*args)
    start = time.perf_counter()
    for _ in range(iterations):
        func(*args)
    elapsed = (time.perf_counter() - start) / iterations * 1000
    print(f"  {label}: {len(result)} results, {elapsed:.2f} ms/query")
    return result


def assert_eq(actual, expected, msg):
    if actual != expected:
        print(f"  FAIL: {msg}: expected {expected}, got {actual}")
        return False
    print(f"  OK: {msg}")
    return True


def assert_in(item, collection, msg):
    if item not in collection:
        print(f"  FAIL: {msg}: {item} not found")
        return False
    print(f"  OK: {msg}")
    return True


def assert_gt(actual, threshold, msg):
    if actual <= threshold:
        print(f"  FAIL: {msg}: expected > {threshold}, got {actual}")
        return False
    print(f"  OK: {msg}")
    return True


def assert_lt(actual, threshold, msg):
    if actual >= threshold:
        print(f"  FAIL: {msg}: expected < {threshold}, got {actual}")
        return False
    print(f"  OK: {msg}")
    return True


def count(conn, table):
    return conn.execute(f"SELECT COUNT(*) FROM [{table}]").fetchone()[0]


def has_column(conn, table, column):
    cols = [r[1] for r in conn.execute(f"PRAGMA table_info([{table}])").fetchall()]
    return column in cols


# --- Correctness tests ---

def test_correctness(conn):
    """Verify query results against known data."""
    passed = 0
    failed = 0

    def check(result):
        nonlocal passed, failed
        if result:
            passed += 1
        else:
            failed += 1

    print("=== Correctness Tests ===\n")

    # --- Airports ---
    print("Airports (NYC area):")
    airports = query_airports(conn, -74.5, 40.0, -73.0, 41.0)
    ids = {a[0] for a in airports}
    check(assert_in("JFK", ids, "JFK in NYC bbox"))
    check(assert_in("LGA", ids, "LGA in NYC bbox"))
    check(assert_in("EWR", ids, "EWR in NYC bbox"))
    check(assert_eq("LAX" in ids, False, "LAX not in NYC bbox"))

    airports_atl = query_airports(conn, -84.5, 33.5, -84.3, 33.8)
    ids_atl = {a[0] for a in airports_atl}
    check(assert_in("ATL", ids_atl, "ATL in Atlanta bbox"))

    # --- Airport detail tables ---
    print("\nAirport details:")
    # Runways: JFK should have runways
    jfk = conn.execute("SELECT SITE_NO FROM APT_BASE WHERE ARPT_ID = 'JFK'").fetchone()
    if jfk:
        rwy_count = conn.execute(
            "SELECT COUNT(*) FROM APT_RWY WHERE SITE_NO = ?", (jfk[0],)
        ).fetchone()[0]
        check(assert_gt(rwy_count, 2, "JFK has multiple runways"))

        rwy_end_count = conn.execute(
            "SELECT COUNT(*) FROM APT_RWY_END WHERE SITE_NO = ?", (jfk[0],)
        ).fetchone()[0]
        check(assert_gt(rwy_end_count, rwy_count, "JFK runway ends > runways"))

        # Airport remarks
        rmk_count = conn.execute(
            "SELECT COUNT(*) FROM APT_RMK WHERE SITE_NO = ?", (jfk[0],)
        ).fetchone()[0]
        check(assert_gt(rmk_count, 0, "JFK has remarks"))

    # Runway rendering segments
    check(assert_gt(count(conn, "RWY_SEG"), 5000, "RWY_SEG has rendering segments"))

    # CLS_ARSP: airport class flags
    check(assert_gt(count(conn, "CLS_ARSP"), 500, "CLS_ARSP has airport class flags"))

    # APT_ATT: attendance schedules
    check(assert_gt(count(conn, "APT_ATT"), 1000, "APT_ATT has attendance data"))

    # --- Navaids ---
    print("\nNavaids (NYC area):")
    navaids = query_navaids(conn, -74.5, 40.0, -73.0, 41.0)
    nav_ids = {n[0] for n in navaids}
    check(assert_in("JFK", nav_ids, "JFK VOR in NYC bbox"))
    check(assert_gt(len(navaids), 0, "at least one navaid in NYC"))

    # Navaid detail tables
    check(assert_gt(count(conn, "NAV_RMK"), 100, "NAV_RMK has remarks"))
    check(assert_gt(count(conn, "NAV_CKPT"), 10, "NAV_CKPT has checkpoints"))

    # --- Fixes ---
    print("\nFixes (NYC area):")
    fixes = query_fixes(conn, -74.5, 40.0, -73.0, 41.0)
    check(assert_gt(len(fixes), 100, "many fixes in NYC area"))

    fixes_ocean = query_fixes(conn, -40.0, 30.0, -39.0, 31.0)
    check(assert_lt(len(fixes_ocean), 10, "few fixes in mid-Atlantic"))

    # Fix detail tables
    check(assert_gt(count(conn, "FIX_NAV"), 1000, "FIX_NAV has fix-navaid relationships"))
    check(assert_gt(count(conn, "FIX_CHRT"), 1000, "FIX_CHRT has chart references"))

    # --- Airways ---
    print("\nAirways (NYC area):")
    airways = query_airways(conn, -74.5, 40.0, -73.0, 41.0)
    check(assert_gt(len(airways), 50, "many airway segments in NYC area"))
    for seg in airways[:5]:
        check(assert_gt(abs(seg[3]), 0.1, f"airway {seg[0]} FROM_LAT nonzero"))

    # Airway metadata
    check(assert_gt(count(conn, "AWY_BASE"), 100, "AWY_BASE has airway records"))

    # --- ILS ---
    print("\nILS:")
    check(assert_gt(count(conn, "ILS_BASE"), 500, "ILS_BASE has ILS records"))
    check(assert_gt(count(conn, "ILS_GS"), 100, "ILS_GS has glide slope data"))
    check(assert_gt(count(conn, "ILS_DME"), 100, "ILS_DME has DME data"))
    check(assert_gt(count(conn, "ILS_MKR"), 50, "ILS_MKR has marker beacons"))
    check(assert_gt(count(conn, "ILS_RMK"), 100, "ILS_RMK has remarks"))
    # JFK should have ILS approaches
    if jfk:
        ils = conn.execute(
            "SELECT COUNT(*) FROM ILS_BASE WHERE SITE_NO = ?", (jfk[0],)
        ).fetchone()[0]
        check(assert_gt(ils, 0, "JFK has ILS approaches"))

    # --- ATC ---
    print("\nATC:")
    check(assert_gt(count(conn, "ATC_BASE"), 500, "ATC_BASE has facilities"))
    check(assert_gt(count(conn, "ATC_ATIS"), 100, "ATC_ATIS has ATIS info"))
    check(assert_gt(count(conn, "ATC_RMK"), 100, "ATC_RMK has remarks"))
    check(assert_gt(count(conn, "ATC_SVC"), 100, "ATC_SVC has services"))

    # --- Frequencies and communications ---
    print("\nFrequencies and communications:")
    check(assert_gt(count(conn, "FRQ"), 10000, "FRQ has frequency records"))
    check(assert_gt(count(conn, "COM"), 500, "COM has comm outlet locations"))
    check(assert_gt(count(conn, "FSS_BASE"), 10, "FSS_BASE has flight service stations"))

    # --- Weather ---
    print("\nWeather:")
    check(assert_gt(count(conn, "AWOS"), 1000, "AWOS has weather stations"))
    check(assert_gt(count(conn, "WXL_BASE"), 1000, "WXL_BASE has weather locations"))
    check(assert_gt(count(conn, "WXL_SVC"), 1000, "WXL_SVC has weather services"))

    # --- Holding patterns ---
    print("\nHolding patterns:")
    check(assert_gt(count(conn, "HPF_BASE"), 10000, "HPF_BASE has holding patterns"))
    check(assert_gt(count(conn, "HPF_SPD_ALT"), 1000, "HPF_SPD_ALT has speed/alt data"))
    check(assert_gt(count(conn, "HPF_CHRT"), 1000, "HPF_CHRT has chart references"))

    # --- Procedures and routes ---
    print("\nProcedures and routes:")
    check(assert_gt(count(conn, "DP_BASE"), 500, "DP_BASE has departure procedures"))
    check(assert_gt(count(conn, "DP_APT"), 1000, "DP_APT has SID airport links"))
    check(assert_gt(count(conn, "DP_RTE"), 1000, "DP_RTE has SID route points"))
    check(assert_gt(count(conn, "STAR_BASE"), 300, "STAR_BASE has STARs"))
    check(assert_gt(count(conn, "STAR_APT"), 1000, "STAR_APT has STAR airport links"))
    check(assert_gt(count(conn, "STAR_RTE"), 1000, "STAR_RTE has STAR route points"))
    check(assert_gt(count(conn, "PFR_BASE"), 5000, "PFR_BASE has preferred routes"))
    check(assert_gt(count(conn, "PFR_SEG"), 10000, "PFR_SEG has route segments"))
    check(assert_gt(count(conn, "CDR"), 10000, "CDR has coded departure routes"))

    # --- Military training routes ---
    print("\nMilitary training routes:")
    check(assert_gt(count(conn, "MTR_BASE"), 100, "MTR_BASE has MTR records"))
    check(assert_gt(count(conn, "MTR_SEG"), 1000, "MTR_SEG has MTR segments"))

    # --- MAA / PJA ---
    print("\nActivity areas:")
    check(assert_gt(count(conn, "MAA_BASE"), 50, "MAA_BASE has activity areas"))
    check(assert_gt(count(conn, "MAA_SHP"), 100, "MAA_SHP has shape points"))
    check(assert_gt(count(conn, "PJA_BASE"), 100, "PJA_BASE has parachute jump areas"))

    # --- Class airspace ---
    print("\nClass Airspace (NYC area):")
    cls_arsp = query_class_airspace(conn, -74.5, 40.0, -73.0, 41.0)
    classes = {a[2] for a in cls_arsp}
    check(assert_in("B", classes, "Class B in NYC bbox"))
    nyc_b = [a for a in cls_arsp if a[2] == "B" and "NEW YORK" in a[1]]
    check(assert_gt(len(nyc_b), 5, "NYC Class B has multiple tiers"))
    surface_b = [a for a in nyc_b if a[4] == "0"]
    check(assert_gt(len(surface_b), 0, "NYC Class B has surface tier"))
    # Metadata columns
    check(assert_eq(has_column(conn, "CLS_ARSP_BASE", "WKHR_CODE"), True,
                    "CLS_ARSP_BASE has WKHR_CODE column"))
    check(assert_eq(has_column(conn, "CLS_ARSP_BASE", "SECTOR"), True,
                    "CLS_ARSP_BASE has SECTOR column"))

    print("\nClass Airspace (ATL area):")
    cls_arsp_atl = query_class_airspace(conn, -84.8, 33.3, -84.0, 34.0)
    atl_b = [a for a in cls_arsp_atl if a[2] == "B" and "ATLANTA" in a[1]]
    check(assert_gt(len(atl_b), 0, "Atlanta Class B present"))

    cls_arsp_lax = query_class_airspace(conn, -118.8, 33.5, -117.8, 34.2)
    lax_b = [a for a in cls_arsp_lax if a[2] == "B" and "LOS ANGELES" in a[1]]
    check(assert_gt(len(lax_b), 0, "LAX Class B present"))

    # Shape integrity
    print("\nClass Airspace shape integrity:")
    if nyc_b:
        arsp_id = nyc_b[0][0]
        points = conn.execute(
            "SELECT PART_NUM, LON_DECIMAL, LAT_DECIMAL FROM CLS_ARSP_SHP "
            "WHERE ARSP_ID = ? ORDER BY PART_NUM, POINT_SEQ",
            (arsp_id,),
        ).fetchall()
        check(assert_gt(len(points), 3, f"ARSP_ID {arsp_id} has shape points"))
        part0 = [p for p in points if p[0] == 0]
        if len(part0) > 2:
            dist = abs(part0[0][1] - part0[-1][1]) + abs(part0[0][2] - part0[-1][2])
            check(assert_lt(dist, 0.01, f"ARSP_ID {arsp_id} ring is closed"))

    # --- SUA ---
    print("\nSUA (eastern US):")
    suas = query_sua(conn, -85.0, 33.0, -75.0, 40.0)
    sua_types = {s[3] for s in suas}
    check(assert_in("MOA", sua_types, "MOAs in eastern US"))
    check(assert_in("RA", sua_types, "Restricted areas in eastern US"))
    check(assert_gt(len(suas), 50, "many SUAs in eastern US"))
    if suas:
        sua_id = suas[0][0]
        pts = conn.execute(
            "SELECT COUNT(*) FROM SUA_SHP WHERE SUA_ID = ?", (sua_id,)
        ).fetchone()[0]
        check(assert_gt(pts, 2, f"SUA_ID {sua_id} has shape points"))

    # SUA metadata columns
    print("\nSUA metadata:")
    check(assert_eq(has_column(conn, "SUA_BASE", "CONTROLLING_AUTHORITY"), True,
                    "SUA_BASE has CONTROLLING_AUTHORITY"))
    check(assert_eq(has_column(conn, "SUA_BASE", "WORKING_HOURS"), True,
                    "SUA_BASE has WORKING_HOURS"))
    check(assert_eq(has_column(conn, "SUA_BASE", "LEGAL_NOTE"), True,
                    "SUA_BASE has LEGAL_NOTE"))
    # Spot-check: restricted areas should have controlling authority
    ra_with_auth = conn.execute("""
        SELECT COUNT(*) FROM SUA_BASE
        WHERE SUA_TYPE = 'RA' AND CONTROLLING_AUTHORITY <> ''
    """).fetchone()[0]
    check(assert_gt(ra_with_auth, 50, "restricted areas have controlling authority"))
    # Working hours should be populated for most
    with_wkhr = conn.execute("""
        SELECT COUNT(*) FROM SUA_BASE WHERE WORKING_HOURS <> ''
    """).fetchone()[0]
    check(assert_gt(with_wkhr, count(conn, "SUA_BASE") // 2,
                    "majority of SUAs have working hours"))

    # --- ARTCC ---
    print("\nARTCC boundaries:")
    check(assert_gt(count(conn, "ARTCC_BASE"), 20, "ARTCC_BASE has boundaries"))
    check(assert_gt(count(conn, "ARTCC_SHP"), 1000, "ARTCC_SHP has shape points"))

    # --- ADIZ ---
    print("\nADIZ:")
    check(assert_gt(count(conn, "ADIZ_BASE"), 5, "ADIZ_BASE has zones"))
    check(assert_gt(count(conn, "ADIZ_SHP"), 1000, "ADIZ_SHP has shape points"))
    check(assert_eq(has_column(conn, "ADIZ_BASE", "WORKING_HOURS"), True,
                    "ADIZ_BASE has WORKING_HOURS"))
    check(assert_eq(has_column(conn, "ADIZ_BASE", "LOCATION"), True,
                    "ADIZ_BASE has LOCATION"))

    # --- Obstacles ---
    print("\nObstacles:")
    check(assert_gt(count(conn, "OBS_BASE"), 500000, "OBS_BASE has >500k obstacles"))
    # OAS_NUM should be primary key
    obs = conn.execute(
        "SELECT OAS_NUM, STATE, CITY, OBSTACLE_TYPE, AGL_HT, HORIZ_ACC, FAA_STUDY "
        "FROM OBS_BASE WHERE OAS_NUM = '01-001307'"
    ).fetchone()
    if obs:
        check(assert_eq(obs[1], "AL", "obstacle 01-001307 is in Alabama"))
        check(assert_gt(obs[4], 0, "obstacle 01-001307 has AGL height"))
        check(assert_eq(obs[3] != "", True, "obstacle 01-001307 has type"))
    else:
        check(assert_eq(True, False, "obstacle 01-001307 lookup by PK"))

    # Spatial query: NYC area should have towers
    obs_nyc = query_obstacles(conn, -74.5, 40.0, -73.0, 41.0)
    check(assert_gt(len(obs_nyc), 100, "many obstacles in NYC area"))
    obs_types = {o[1] for o in obs_nyc}
    check(assert_gt(len(obs_types), 3, "multiple obstacle types in NYC"))

    # --- R-tree exclusion ---
    print("\nR-tree exclusion:")
    alaska = query_airports(conn, -152.0, 60.0, -148.0, 62.0)
    alaska_ids = {a[0] for a in alaska}
    check(assert_eq("JFK" in alaska_ids, False, "JFK not in Alaska bbox"))
    check(assert_eq("ATL" in alaska_ids, False, "ATL not in Alaska bbox"))

    print(f"\n--- {passed} passed, {failed} failed ---")
    return failed == 0


# --- Performance tests ---

def test_performance(conn):
    """Benchmark query performance at various viewport sizes."""
    print("\n=== Performance Tests ===\n")

    print("NYC area (-74.5, 40.0, -73.0, 41.0):")
    bbox = (-74.5, 40.0, -73.0, 41.0)
    benchmark("airports", query_airports, conn, *bbox)
    benchmark("navaids", query_navaids, conn, *bbox)
    benchmark("fixes", query_fixes, conn, *bbox)
    benchmark("airways", query_airways, conn, *bbox)
    benchmark("maa", query_maa, conn, *bbox)
    benchmark("class_airspace", query_class_airspace, conn, *bbox)
    benchmark("sua", query_sua, conn, *bbox)
    benchmark("obstacles", query_obstacles, conn, *bbox)
    bases, pts = query_class_airspace_with_shapes(conn, *bbox)
    start = time.perf_counter()
    for _ in range(20):
        query_class_airspace_with_shapes(conn, *bbox)
    elapsed = (time.perf_counter() - start) / 20 * 1000
    print(f"  class_airspace+shapes: {len(bases)} areas, {pts} points, {elapsed:.2f} ms/query")

    print("\nCONUS-wide (-125, 24, -66, 50):")
    bbox = (-125.0, 24.0, -66.0, 50.0)
    benchmark("airports", query_airports, conn, *bbox)
    benchmark("navaids", query_navaids, conn, *bbox)
    benchmark("fixes", query_fixes, conn, *bbox)
    benchmark("airways", query_airways, conn, *bbox)
    benchmark("maa", query_maa, conn, *bbox)
    benchmark("class_airspace", query_class_airspace, conn, *bbox)
    benchmark("obstacles", query_obstacles, conn, *bbox, iterations=10)

    print("\nSmall area around KATL (-84.5, 33.5, -84.3, 33.7):")
    bbox = (-84.5, 33.5, -84.3, 33.7)
    benchmark("airports", query_airports, conn, *bbox)
    benchmark("navaids", query_navaids, conn, *bbox)
    benchmark("fixes", query_fixes, conn, *bbox)
    benchmark("airways", query_airways, conn, *bbox)
    benchmark("maa", query_maa, conn, *bbox)
    benchmark("class_airspace", query_class_airspace, conn, *bbox)
    benchmark("obstacles", query_obstacles, conn, *bbox)


def main():
    db_path = sys.argv[1] if len(sys.argv) > 1 else "nasr.db"
    conn = sqlite3.connect(db_path)

    all_passed = test_correctness(conn)
    test_performance(conn)

    conn.close()
    sys.exit(0 if all_passed else 1)


if __name__ == "__main__":
    main()
