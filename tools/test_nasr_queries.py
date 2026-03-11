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


def benchmark(label, func, *args, iterations=100):
    # Warm up
    result = func(*args)
    start = time.perf_counter()
    for _ in range(iterations):
        func(*args)
    elapsed = (time.perf_counter() - start) / iterations * 1000
    print(f"  {label}: {len(result)} results, {elapsed:.2f} ms/query")
    return result


def main():
    db_path = sys.argv[1] if len(sys.argv) > 1 else "nasr.db"
    conn = sqlite3.connect(db_path)

    # Test 1: NYC area (dense)
    print("NYC area (-74.5, 40.0, -73.0, 41.0):")
    bbox = (-74.5, 40.0, -73.0, 41.0)
    airports = benchmark("airports", query_airports, conn, *bbox)
    benchmark("navaids", query_navaids, conn, *bbox)
    benchmark("fixes", query_fixes, conn, *bbox)
    benchmark("airways", query_airways, conn, *bbox)
    benchmark("airspaces", query_airspaces, conn, *bbox)

    # Verify known airports
    ids = {a[0] for a in airports}
    for expected in ["JFK", "LGA", "EWR"]:
        status = "OK" if expected in ids else "MISSING"
        print(f"  {expected}: {status}")

    # Test 2: CONUS-wide (many results)
    print("\nCONUS-wide (-125, 24, -66, 50):")
    bbox = (-125.0, 24.0, -66.0, 50.0)
    benchmark("airports", query_airports, conn, *bbox)
    benchmark("navaids", query_navaids, conn, *bbox)
    benchmark("fixes", query_fixes, conn, *bbox)
    benchmark("airways", query_airways, conn, *bbox)
    benchmark("airspaces", query_airspaces, conn, *bbox)

    # Test 3: Small area (few results)
    print("\nSmall area around KATL (-84.5, 33.5, -84.3, 33.7):")
    bbox = (-84.5, 33.5, -84.3, 33.7)
    benchmark("airports", query_airports, conn, *bbox)
    benchmark("navaids", query_navaids, conn, *bbox)
    benchmark("fixes", query_fixes, conn, *bbox)
    benchmark("airways", query_airways, conn, *bbox)
    benchmark("airspaces", query_airspaces, conn, *bbox)

    conn.close()


if __name__ == "__main__":
    main()
