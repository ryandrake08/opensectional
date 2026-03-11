#!/usr/bin/env python3
"""Build SQLite database from FAA NASR CSV data.

Imports key tables, adds decimal coordinates where needed,
and creates R-tree spatial indexes for efficient bounding box queries.

Usage:
    python3 build_nasr_db.py <csv_dir> <output.db>

Example:
    python3 build_nasr_db.py nasr_data/19_Feb_2026_CSV nasr.db
"""

import csv
import math
import os
import re
import sqlite3
import sys


def parse_dms(dms_str):
    """Parse DD-MM-SS.SSSSH format to decimal degrees.

    Examples: "33-54-12.8500N" -> 33.903569
              "087-19-53.7600W" -> -87.331600
    """
    m = re.match(r"(\d+)-(\d+)-([\d.]+)([NSEW])", dms_str)
    if not m:
        return None
    deg = int(m.group(1))
    mins = int(m.group(2))
    secs = float(m.group(3))
    hem = m.group(4)
    decimal = deg + mins / 60.0 + secs / 3600.0
    if hem in ("S", "W"):
        decimal = -decimal
    return decimal


def import_csv(conn, table_name, csv_path, columns=None):
    """Import a CSV file into a SQLite table.

    If columns is None, imports all columns. Otherwise, imports only
    the specified columns.
    """
    with open(csv_path, "r", encoding="latin-1") as f:
        reader = csv.DictReader(f)
        all_cols = reader.fieldnames
        if columns is None:
            columns = all_cols

        # Verify all requested columns exist
        for col in columns:
            if col not in all_cols:
                print(f"  Warning: column '{col}' not found in {csv_path}")
                columns = [c for c in columns if c in all_cols]

        col_defs = ", ".join(f'"{c}" TEXT' for c in columns)
        conn.execute(f"DROP TABLE IF EXISTS {table_name}")
        conn.execute(f"CREATE TABLE {table_name} ({col_defs})")

        placeholders = ", ".join("?" for _ in columns)
        insert_sql = f"INSERT INTO {table_name} VALUES ({placeholders})"

        rows = []
        for row in reader:
            values = [row.get(c, "") for c in columns]
            rows.append(values)

        conn.executemany(insert_sql, rows)
        print(f"  {table_name}: {len(rows)} rows")


def build_apt(conn, csv_dir):
    """Import airports with decimal coordinates."""
    import_csv(conn, "APT_BASE", os.path.join(csv_dir, "APT_BASE.csv"), [
        "SITE_NO", "SITE_TYPE_CODE", "STATE_CODE", "ARPT_ID", "CITY",
        "COUNTRY_CODE", "ARPT_NAME", "OWNERSHIP_TYPE_CODE",
        "FACILITY_USE_CODE", "LAT_DECIMAL", "LONG_DECIMAL", "ELEV",
        "TWR_TYPE_CODE", "ICAO_ID", "ARPT_STATUS",
    ])

    # R-tree index on lat/lon
    conn.execute("""
        CREATE VIRTUAL TABLE APT_BASE_RTREE USING rtree(
            id, min_lon, max_lon, min_lat, max_lat
        )
    """)
    conn.execute("""
        INSERT INTO APT_BASE_RTREE (id, min_lon, max_lon, min_lat, max_lat)
        SELECT rowid,
               CAST(LONG_DECIMAL AS REAL), CAST(LONG_DECIMAL AS REAL),
               CAST(LAT_DECIMAL AS REAL), CAST(LAT_DECIMAL AS REAL)
        FROM APT_BASE
        WHERE LAT_DECIMAL IS NOT NULL AND LONG_DECIMAL IS NOT NULL
            AND LAT_DECIMAL != '' AND LONG_DECIMAL != ''
    """)


def build_nav(conn, csv_dir):
    """Import navaids with decimal coordinates."""
    import_csv(conn, "NAV_BASE", os.path.join(csv_dir, "NAV_BASE.csv"), [
        "NAV_ID", "NAV_TYPE", "STATE_CODE", "CITY", "COUNTRY_CODE",
        "NAV_STATUS", "NAME", "LAT_DECIMAL", "LONG_DECIMAL",
        "ELEV", "FREQ", "CHAN",
    ])

    conn.execute("""
        CREATE VIRTUAL TABLE NAV_BASE_RTREE USING rtree(
            id, min_lon, max_lon, min_lat, max_lat
        )
    """)
    conn.execute("""
        INSERT INTO NAV_BASE_RTREE (id, min_lon, max_lon, min_lat, max_lat)
        SELECT rowid,
               CAST(LONG_DECIMAL AS REAL), CAST(LONG_DECIMAL AS REAL),
               CAST(LAT_DECIMAL AS REAL), CAST(LAT_DECIMAL AS REAL)
        FROM NAV_BASE
        WHERE LAT_DECIMAL IS NOT NULL AND LONG_DECIMAL IS NOT NULL
            AND LAT_DECIMAL != '' AND LONG_DECIMAL != ''
    """)


def build_fix(conn, csv_dir):
    """Import fixes with decimal coordinates."""
    import_csv(conn, "FIX_BASE", os.path.join(csv_dir, "FIX_BASE.csv"), [
        "FIX_ID", "STATE_CODE", "COUNTRY_CODE", "LAT_DECIMAL",
        "LONG_DECIMAL", "FIX_USE_CODE",
    ])

    conn.execute("""
        CREATE VIRTUAL TABLE FIX_BASE_RTREE USING rtree(
            id, min_lon, max_lon, min_lat, max_lat
        )
    """)
    conn.execute("""
        INSERT INTO FIX_BASE_RTREE (id, min_lon, max_lon, min_lat, max_lat)
        SELECT rowid,
               CAST(LONG_DECIMAL AS REAL), CAST(LONG_DECIMAL AS REAL),
               CAST(LAT_DECIMAL AS REAL), CAST(LAT_DECIMAL AS REAL)
        FROM FIX_BASE
        WHERE LAT_DECIMAL IS NOT NULL AND LONG_DECIMAL IS NOT NULL
            AND LAT_DECIMAL != '' AND LONG_DECIMAL != ''
    """)


def build_awy(conn, csv_dir):
    """Import airways and build segment table with resolved coordinates.

    AWY_SEG_ALT references waypoints by name. We resolve FROM_POINT
    coordinates by joining against NAV_BASE and FIX_BASE.
    """
    import_csv(conn, "AWY_SEG_ALT", os.path.join(csv_dir, "AWY_SEG_ALT.csv"), [
        "AWY_LOCATION", "AWY_ID", "POINT_SEQ", "FROM_POINT", "FROM_PT_TYPE",
        "COUNTRY_CODE", "TO_POINT", "MAG_COURSE_DIST",
        "AWY_SEG_GAP_FLAG", "MIN_ENROUTE_ALT",
    ])

    # Build a waypoint lookup table combining navaids and fixes
    conn.execute("""
        CREATE TABLE WP_LOOKUP AS
        SELECT NAV_ID AS WP_ID, 'NAV' AS WP_SOURCE,
               CAST(LAT_DECIMAL AS REAL) AS LAT,
               CAST(LONG_DECIMAL AS REAL) AS LON
        FROM NAV_BASE
        WHERE LAT_DECIMAL IS NOT NULL AND LONG_DECIMAL IS NOT NULL
        UNION ALL
        SELECT FIX_ID AS WP_ID, 'FIX' AS WP_SOURCE,
               CAST(LAT_DECIMAL AS REAL) AS LAT,
               CAST(LONG_DECIMAL AS REAL) AS LON
        FROM FIX_BASE
        WHERE LAT_DECIMAL IS NOT NULL AND LONG_DECIMAL IS NOT NULL
        UNION ALL
        SELECT ARPT_ID AS WP_ID, 'APT' AS WP_SOURCE,
               CAST(LAT_DECIMAL AS REAL) AS LAT,
               CAST(LONG_DECIMAL AS REAL) AS LON
        FROM APT_BASE
        WHERE LAT_DECIMAL IS NOT NULL AND LONG_DECIMAL IS NOT NULL
    """)
    conn.execute("CREATE INDEX idx_wp_lookup ON WP_LOOKUP(WP_ID)")

    # Build resolved airway segments with coordinates
    # Each segment is a FROM->TO pair with lat/lon for both endpoints
    conn.execute("""
        CREATE TABLE AWY_SEG AS
        SELECT
            s.AWY_ID,
            s.AWY_LOCATION,
            CAST(s.POINT_SEQ AS INTEGER) AS POINT_SEQ,
            s.FROM_POINT,
            s.TO_POINT,
            wf.LAT AS FROM_LAT,
            wf.LON AS FROM_LON,
            wt.LAT AS TO_LAT,
            wt.LON AS TO_LON,
            s.AWY_SEG_GAP_FLAG,
            s.MIN_ENROUTE_ALT
        FROM AWY_SEG_ALT s
        LEFT JOIN WP_LOOKUP wf ON wf.WP_ID = s.FROM_POINT
        LEFT JOIN WP_LOOKUP wt ON wt.WP_ID = s.TO_POINT
        WHERE wf.LAT IS NOT NULL AND wt.LAT IS NOT NULL
    """)

    # Handle duplicate waypoint names by picking the closest match
    # For now, just deduplicate by taking the first match
    # (The LEFT JOIN may produce duplicates if WP_LOOKUP has multiple entries)
    conn.execute("DROP TABLE IF EXISTS AWY_SEG_DEDUP")
    conn.execute("""
        CREATE TABLE AWY_SEG_DEDUP AS
        SELECT AWY_ID, AWY_LOCATION, POINT_SEQ, FROM_POINT, TO_POINT,
               FROM_LAT, FROM_LON, TO_LAT, TO_LON,
               AWY_SEG_GAP_FLAG, MIN_ENROUTE_ALT
        FROM AWY_SEG
        GROUP BY AWY_ID, AWY_LOCATION, POINT_SEQ
    """)
    conn.execute("DROP TABLE AWY_SEG")
    conn.execute("ALTER TABLE AWY_SEG_DEDUP RENAME TO AWY_SEG")

    count = conn.execute("SELECT COUNT(*) FROM AWY_SEG").fetchone()[0]
    print(f"  AWY_SEG (resolved): {count} segments")

    # R-tree on segment bounding boxes
    conn.execute("""
        CREATE VIRTUAL TABLE AWY_SEG_RTREE USING rtree(
            id, min_lon, max_lon, min_lat, max_lat
        )
    """)
    conn.execute("""
        INSERT INTO AWY_SEG_RTREE (id, min_lon, max_lon, min_lat, max_lat)
        SELECT rowid,
               MIN(FROM_LON, TO_LON), MAX(FROM_LON, TO_LON),
               MIN(FROM_LAT, TO_LAT), MAX(FROM_LAT, TO_LAT)
        FROM AWY_SEG
    """)


def build_maa(conn, csv_dir):
    """Import MOA/SUA data with polygon shapes."""
    import_csv(conn, "MAA_BASE", os.path.join(csv_dir, "MAA_BASE.csv"), [
        "MAA_ID", "MAA_TYPE_NAME", "MAA_NAME", "MAX_ALT", "MIN_ALT",
    ])

    # Import shape points and convert DMS to decimal
    import_csv(conn, "MAA_SHP_RAW", os.path.join(csv_dir, "MAA_SHP.csv"), [
        "MAA_ID", "POINT_SEQ", "LATITUDE", "LONGITUDE",
    ])

    # Parse DMS coordinates
    conn.execute("""
        CREATE TABLE MAA_SHP AS
        SELECT MAA_ID,
               CAST(POINT_SEQ AS INTEGER) AS POINT_SEQ,
               LATITUDE AS LAT_DMS,
               LONGITUDE AS LON_DMS,
               0.0 AS LAT_DECIMAL,
               0.0 AS LON_DECIMAL
        FROM MAA_SHP_RAW
    """)
    conn.execute("DROP TABLE MAA_SHP_RAW")

    # Update with parsed decimal coordinates
    cursor = conn.execute("SELECT rowid, LAT_DMS, LON_DMS FROM MAA_SHP")
    updates = []
    for row in cursor:
        lat = parse_dms(row[1])
        lon = parse_dms(row[2])
        if lat is not None and lon is not None:
            updates.append((lat, lon, row[0]))
    conn.executemany(
        "UPDATE MAA_SHP SET LAT_DECIMAL = ?, LON_DECIMAL = ? WHERE rowid = ?",
        updates,
    )
    print(f"  MAA_SHP: {len(updates)} shape points converted")

    # R-tree for MAA_BASE using bounding box of shape points
    conn.execute("""
        CREATE VIRTUAL TABLE MAA_BASE_RTREE USING rtree(
            id, min_lon, max_lon, min_lat, max_lat
        )
    """)
    conn.execute("""
        INSERT INTO MAA_BASE_RTREE (id, min_lon, max_lon, min_lat, max_lat)
        SELECT b.rowid,
               MIN(s.LON_DECIMAL), MAX(s.LON_DECIMAL),
               MIN(s.LAT_DECIMAL), MAX(s.LAT_DECIMAL)
        FROM MAA_BASE b
        JOIN MAA_SHP s ON s.MAA_ID = b.MAA_ID
        WHERE s.LAT_DECIMAL != 0.0
        GROUP BY b.rowid
    """)


def build_apt_rwy(conn, csv_dir):
    """Import runway data for high-zoom rendering."""
    import_csv(conn, "APT_RWY", os.path.join(csv_dir, "APT_RWY.csv"), [
        "SITE_NO", "RWY_ID", "RWY_LEN", "RWY_WIDTH", "SURFACE_TYPE_CODE",
    ])

    import_csv(conn, "APT_RWY_END", os.path.join(csv_dir, "APT_RWY_END.csv"), [
        "SITE_NO", "RWY_ID", "RWY_END_ID", "LAT_DECIMAL", "LONG_DECIMAL",
        "RWY_END_ELEV", "TRUE_ALIGNMENT",
    ])



def main():
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <csv_dir> <output.db>")
        sys.exit(1)

    csv_dir = sys.argv[1]
    db_path = sys.argv[2]

    if not os.path.isdir(csv_dir):
        print(f"Error: {csv_dir} is not a directory")
        sys.exit(1)

    # Remove existing database
    if os.path.exists(db_path):
        os.remove(db_path)

    conn = sqlite3.connect(db_path)
    conn.execute("PRAGMA journal_mode=WAL")
    conn.execute("PRAGMA synchronous=NORMAL")

    print("Building NASR database...")

    print("Importing airports...")
    build_apt(conn, csv_dir)

    print("Importing navaids...")
    build_nav(conn, csv_dir)

    print("Importing fixes...")
    build_fix(conn, csv_dir)

    print("Importing airways...")
    build_awy(conn, csv_dir)

    print("Importing MOA/SUA...")
    build_maa(conn, csv_dir)

    print("Importing runway data...")
    build_apt_rwy(conn, csv_dir)

    conn.commit()

    # Print summary
    print("\nDatabase summary:")
    for table in ["APT_BASE", "NAV_BASE", "FIX_BASE", "AWY_SEG", "MAA_BASE", "MAA_SHP", "APT_RWY", "APT_RWY_END"]:
        count = conn.execute(f"SELECT COUNT(*) FROM {table}").fetchone()[0]
        print(f"  {table}: {count} rows")

    conn.close()
    db_size = os.path.getsize(db_path)
    print(f"\nDatabase written to {db_path} ({db_size / 1024 / 1024:.1f} MB)")


if __name__ == "__main__":
    main()
