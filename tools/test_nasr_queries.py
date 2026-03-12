#!/usr/bin/env python3
"""Test NASR database queries for correctness and performance."""

import sqlite3
import sys
import time


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


def query_airspaces(conn, lon_min, lat_min, lon_max, lat_max):
    return conn.execute("""
        SELECT MAA_ID, MAA_TYPE_NAME, MAA_NAME
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

    # Airports: NYC area should contain JFK, LGA, EWR
    print("Airports (NYC area):")
    airports = query_airports(conn, -74.5, 40.0, -73.0, 41.0)
    ids = {a[0] for a in airports}
    check(assert_in("JFK", ids, "JFK in NYC bbox"))
    check(assert_in("LGA", ids, "LGA in NYC bbox"))
    check(assert_in("EWR", ids, "EWR in NYC bbox"))

    # Airports: LAX should not appear in NYC bbox
    check(assert_eq("LAX" in ids, False, "LAX not in NYC bbox"))

    # Airports: ATL area
    airports_atl = query_airports(conn, -84.5, 33.5, -84.3, 33.8)
    ids_atl = {a[0] for a in airports_atl}
    check(assert_in("ATL", ids_atl, "ATL in Atlanta bbox"))

    # Navaids: JFK VOR should be near JFK
    print("\nNavaids (NYC area):")
    navaids = query_navaids(conn, -74.5, 40.0, -73.0, 41.0)
    nav_ids = {n[0] for n in navaids}
    check(assert_in("JFK", nav_ids, "JFK VOR in NYC bbox"))
    check(assert_gt(len(navaids), 0, "at least one navaid in NYC"))

    # Fixes: should have many in NYC area
    print("\nFixes (NYC area):")
    fixes = query_fixes(conn, -74.5, 40.0, -73.0, 41.0)
    check(assert_gt(len(fixes), 100, "many fixes in NYC area"))

    # Fixes: empty ocean area should have few/no fixes
    fixes_ocean = query_fixes(conn, -40.0, 30.0, -39.0, 31.0)
    check(assert_lt(len(fixes_ocean), 10, "few fixes in mid-Atlantic"))

    # Airways: should have segments in NYC area
    print("\nAirways (NYC area):")
    airways = query_airways(conn, -74.5, 40.0, -73.0, 41.0)
    check(assert_gt(len(airways), 50, "many airway segments in NYC area"))
    # Each segment should have valid coordinates
    for seg in airways[:5]:
        check(assert_gt(abs(seg[3]), 0.1, f"airway {seg[0]} FROM_LAT nonzero"))

    # Class airspace: NYC should have Class B
    print("\nClass Airspace (NYC area):")
    cls_arsp = query_class_airspace(conn, -74.5, 40.0, -73.0, 41.0)
    classes = {a[2] for a in cls_arsp}
    check(assert_in("B", classes, "Class B in NYC bbox"))
    # NYC Class B should have multiple altitude tiers
    nyc_b = [a for a in cls_arsp if a[2] == "B" and "NEW YORK" in a[1]]
    check(assert_gt(len(nyc_b), 5, "NYC Class B has multiple tiers"))
    # Surface-level tier (floor = 0)
    surface_b = [a for a in nyc_b if a[4] == "0"]
    check(assert_gt(len(surface_b), 0, "NYC Class B has surface tier"))

    # Class airspace: ATL should have Class B
    print("\nClass Airspace (ATL area):")
    cls_arsp_atl = query_class_airspace(conn, -84.8, 33.3, -84.0, 34.0)
    atl_b = [a for a in cls_arsp_atl if a[2] == "B" and "ATLANTA" in a[1]]
    check(assert_gt(len(atl_b), 0, "Atlanta Class B present"))

    # Class airspace: LAX should have Class B
    cls_arsp_lax = query_class_airspace(conn, -118.8, 33.5, -117.8, 34.2)
    lax_b = [a for a in cls_arsp_lax if a[2] == "B" and "LOS ANGELES" in a[1]]
    check(assert_gt(len(lax_b), 0, "LAX Class B present"))

    # Class airspace: shape points should exist and form closed rings
    print("\nClass Airspace shape integrity:")
    if nyc_b:
        arsp_id = nyc_b[0][0]
        points = conn.execute(
            "SELECT PART_NUM, LON_DECIMAL, LAT_DECIMAL FROM CLS_ARSP_SHP "
            "WHERE ARSP_ID = ? ORDER BY PART_NUM, POINT_SEQ",
            (arsp_id,),
        ).fetchall()
        check(assert_gt(len(points), 3, f"ARSP_ID {arsp_id} has shape points"))
        # First and last point of part 0 should be close (closed ring)
        part0 = [p for p in points if p[0] == 0]
        if len(part0) > 2:
            dist = abs(part0[0][1] - part0[-1][1]) + abs(part0[0][2] - part0[-1][2])
            check(assert_lt(dist, 0.01, f"ARSP_ID {arsp_id} ring is closed"))

    # R-tree should not return results outside the query box
    print("\nR-tree exclusion:")
    # Query a small box in Alaska - should not contain CONUS airports
    alaska = query_airports(conn, -152.0, 60.0, -148.0, 62.0)
    alaska_ids = {a[0] for a in alaska}
    check(assert_eq("JFK" in alaska_ids, False, "JFK not in Alaska bbox"))
    check(assert_eq("ATL" in alaska_ids, False, "ATL not in Alaska bbox"))

    print(f"\n--- {passed} passed, {failed} failed ---")
    return failed == 0


def test_performance(conn):
    """Benchmark query performance at various viewport sizes."""
    print("\n=== Performance Tests ===\n")

    print("NYC area (-74.5, 40.0, -73.0, 41.0):")
    bbox = (-74.5, 40.0, -73.0, 41.0)
    benchmark("airports", query_airports, conn, *bbox)
    benchmark("navaids", query_navaids, conn, *bbox)
    benchmark("fixes", query_fixes, conn, *bbox)
    benchmark("airways", query_airways, conn, *bbox)
    benchmark("airspaces", query_airspaces, conn, *bbox)
    benchmark("class_airspace", query_class_airspace, conn, *bbox)
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
    benchmark("airspaces", query_airspaces, conn, *bbox)
    benchmark("class_airspace", query_class_airspace, conn, *bbox)

    print("\nSmall area around KATL (-84.5, 33.5, -84.3, 33.7):")
    bbox = (-84.5, 33.5, -84.3, 33.7)
    benchmark("airports", query_airports, conn, *bbox)
    benchmark("navaids", query_navaids, conn, *bbox)
    benchmark("fixes", query_fixes, conn, *bbox)
    benchmark("airways", query_airways, conn, *bbox)
    benchmark("airspaces", query_airspaces, conn, *bbox)
    benchmark("class_airspace", query_class_airspace, conn, *bbox)


def main():
    db_path = sys.argv[1] if len(sys.argv) > 1 else "nasr.db"
    conn = sqlite3.connect(db_path)

    all_passed = test_correctness(conn)
    test_performance(conn)

    conn.close()
    sys.exit(0 if all_passed else 1)


if __name__ == "__main__":
    main()
