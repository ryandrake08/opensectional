#!/usr/bin/env python3
"""Build SQLite database from FAA NASR data.

Reads directly from the downloaded ZIP files without requiring
manual extraction. Imports CSV tables, ESRI shapefiles, and AIXM 5.0
XML, adds decimal coordinates where needed, and creates R-tree spatial
indexes for bounding box queries.

Usage:
    python3 build_nasr_db.py <csv.zip> <shapefile.zip> <aixm.zip> <dof.zip> <output.db>

Example:
    python3 build_nasr_db.py 19_Feb_2026_CSV.zip class_airspace_shape_files.zip aixm5.0.zip DOF.zip nasr.db
"""

import csv
import io
import json
import math
import os
import re
import sqlite3
import struct
import sys
import xml.etree.ElementTree as ET
from collections import defaultdict

# Per-SUA instrumentation for the geometry cleanup filters. Each key is
# a filter name; each value is a Counter {designator: hits}. After
# build_sua runs, a summary is printed so we can see which filters still
# earn their keep post-stratum-rework.
SUA_FILTER_STATS = defaultdict(lambda: defaultdict(int))
import zipfile


# Sentinel for "unlimited" altitude. Any real altitude compared against
# this will register as below it. Stored as an INT in the DB.
ALT_UNLIMITED_FT = 99999


def bbox_for_circle(rowid, lon, lat, radius_nm):
    """R-tree bbox tuple enclosing a (lon, lat, radius) circle.

    Pads by the circle radius converted to degrees so the stored bbox fully
    contains the disc — a degenerate-point pick query then matches any circle
    covering the click. Radius 0 (or missing) degenerates to the center point.
    """
    r = radius_nm or 0.0
    lat_pad = r / 60.0
    cos_lat = math.cos(math.radians(lat))
    lon_pad = lat_pad / max(abs(cos_lat), 0.01)
    return (rowid, lon - lon_pad, lon + lon_pad,
            lat - lat_pad, lat + lat_pad)


def parse_altitude(text):
    """Parse an FAA altitude string into a (value_ft, ref) pair.

    Returns one of these refs:
      "MSL"   — value is feet above mean sea level.
      "SFC"   — value is feet above ground level (AGL).
      "STD"   — value is a flight level (pressure altitude in
                hundreds of ft); stored as the equivalent ft number
                (FL180 → 18000) but the STD ref preserves the
                pressure-altitude semantics.
      "OTHER" — no meaningful numeric (UNL/unlimited); value is
                ALT_UNLIMITED_FT as a sentinel.

    Examples (input → (value, ref)):
      "5000 FT MSL"     → (5000,  "MSL")
      "GND FT SFC"      → (0,     "SFC")
      "1500 FT SFC"     → (1500,  "SFC")
      "180 FL STD"      → (18000, "STD")
      "UNL OTHER", "UNL"→ (ALT_UNLIMITED_FT, "OTHER")
      "" / None         → (None, None)

    Two altitudes may be compared numerically only when their refs
    match. The conversion of SFC → MSL requires knowing the ground
    elevation, which we deliberately do not have; comparisons across
    references should be guarded by `same_ref` checks.
    """
    if text is None:
        return (None, None)
    s = str(text).strip().upper()
    if not s:
        return (None, None)
    if s.startswith("UNL"):
        return (ALT_UNLIMITED_FT, "OTHER")
    if s.startswith("GND") or s.startswith("SFC"):
        return (0, "SFC")
    m = re.match(r"(\d+)\s*FL", s)
    if m:
        return (int(m.group(1)) * 100, "STD")
    m = re.match(r"(\d+)", s)
    if m:
        # Numeric without explicit ref — disambiguate via the trailing
        # word. Default to MSL for backward compat with strings that
        # lack a reference token (rare; mainly historical data).
        v = int(m.group(1))
        if "SFC" in s or "AGL" in s:
            return (v, "SFC")
        if "STD" in s or " FL" in s.replace("FT FL", " FL"):
            return (v, "STD")
        return (v, "MSL")
    return (None, None)


def parse_altitude_ft_msl(text):
    """Backward-compat wrapper: returns the int part of `parse_altitude`.

    Loses the reference; only safe to use where the caller already
    accepts the MSL-conflation semantics (e.g. as a sort key or for
    legacy fields). Prefer `parse_altitude` when the reference matters.
    """
    v, _ = parse_altitude(text)
    return v


def parse_dof_dms(dms_str):
    """Parse DOF DMS format to decimal degrees.

    Examples: "32 31 54.66N" -> 32.531850
              " 117 11 11.20W" -> -117.186444
    """
    parts = dms_str.split()
    if len(parts) != 3:
        return None
    sec_hem = parts[2]
    hem = sec_hem[-1]
    decimal = int(parts[0]) + int(parts[1]) / 60.0 + float(sec_hem[:-1]) / 3600.0
    if hem == "S" or hem == "W":
        decimal = -decimal
    return decimal


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


def subdivide_ring(points, max_points=32):
    """Split a polygon ring into overlapping chunks for tighter R-tree bboxes.

    Each chunk has at most max_points points and overlaps the next by 1 point.
    The final chunk wraps back to include the first point to close the ring
    visually.  Returns a list of point lists.
    """
    n = len(points)

    # Ensure ring is closed
    closed = points
    if points[0] != points[-1]:
        closed = points + [points[0]]
        n = len(closed)

    if n <= max_points:
        return [closed]

    stride = max_points - 1  # overlap by 1
    chunks = []
    offset = 0
    while offset < n - 1:
        end = min(offset + max_points, n)
        chunk = closed[offset:end]
        # If this is the last chunk and it doesn't reach the closing point,
        # append the first point to visually close the ring
        if end == n and chunk[-1] != closed[0]:
            chunk.append(closed[0])
        chunks.append(chunk)
        offset += stride

    return chunks


def handle_antimeridian(points):
    """Handle geometry that crosses the antimeridian (±180° longitude).

    If no crossing is detected, returns [points] unchanged.
    If a crossing is detected, returns two copies of the geometry:
    one with longitudes unwrapped to the west (extending past -180)
    and one shifted +360 to the east (extending past +180).
    Both copies are complete rings; viewport culling hides the offscreen one.
    """
    # Detect if any segment crosses (longitude jump > 180°)
    has_crossing = False
    for i in range(len(points) - 1):
        if abs(points[i + 1][0] - points[i][0]) > 180:
            has_crossing = True
            break
    if not has_crossing:
        return [points]

    # Unwrap longitudes to be continuous (no ±360 jumps)
    unwrapped = [points[0]]
    for i in range(1, len(points)):
        lon, lat = points[i]
        prev_lon = unwrapped[i - 1][0]
        while lon - prev_lon > 180:
            lon -= 360
        while lon - prev_lon < -180:
            lon += 360
        unwrapped.append((lon, lat))

    # Create second copy shifted by ±360 to cover the other side
    shifted = [(lon + 360, lat) for lon, lat in unwrapped]
    if unwrapped[0][0] > 0:
        shifted = [(lon - 360, lat) for lon, lat in unwrapped]
        return [shifted, unwrapped]
    return [unwrapped, shifted]


def read_csv_rows(zf, csv_name):
    """Read CSV rows from a ZIP archive as list of dicts."""
    with zf.open(csv_name) as f:
        reader = csv.DictReader(io.TextIOWrapper(f, encoding="utf-8"))
        return list(reader)


def import_csv(conn, table_name, zf, csv_name, columns=None):
    """Import a CSV file from a ZIP archive into a SQLite table.

    If columns is None, imports all columns. Otherwise, imports only
    the specified columns.
    """
    with zf.open(csv_name) as raw:
        f = io.TextIOWrapper(raw, encoding="latin-1")
        reader = csv.reader(f)
        all_cols = next(reader)

        if columns is None:
            columns = list(all_cols)

        # Build index map: requested column -> position in CSV row
        col_index = {c: i for i, c in enumerate(all_cols)}
        for col in columns:
            if col not in col_index:
                print(f"  Warning: column '{col}' not found in {csv_name}")
        columns = [c for c in columns if c in col_index]
        indices = [col_index[c] for c in columns]

        col_defs = ", ".join(f'"{c}" TEXT' for c in columns)
        conn.execute(f"DROP TABLE IF EXISTS {table_name}")
        conn.execute(f"CREATE TABLE {table_name} ({col_defs})")

        placeholders = ", ".join("?" for _ in columns)
        insert_sql = f"INSERT INTO {table_name} VALUES ({placeholders})"

        rows = []
        for row in reader:
            rows.append([row[i].strip() for i in indices])

        conn.executemany(insert_sql, rows)
        print(f"  {table_name}: {len(rows)} rows")


def classify_surface(code):
    """Classify a runway surface type code as HARD, SOFT, or OTHER."""
    hard = {
        "ASPH", "CONC", "ASPH-CONC", "ASPH-TURF", "CONC-TURF", "PEM",
        "CONC-GRVL", "ASPH-TRTD", "ASPH-DIRT", "CONC-TRTD", "PSP",
        "ASPH/GRVL", "ASPH-GRVL", "TREATED", "CONC-DIRT", "OIL&CHIP-T",
    }
    soft = {
        "TURF", "DIRT", "GRVL", "GRAVEL", "TURF-DIRT", "TURF-GRVL",
        "GRVL-DIRT", "GRVL-TRTD", "TRTD", "TRTD-DIRT",
        "GRVL-TURF", "GRASS", "DIRT-TRTD", "DIRT-GRVL", "DIRT-TURF",
        "TURF-SAND", "SAND", "CALICHE", "SNOW", "CORAL",
    }
    if code in hard:
        return "HARD"
    if code in soft:
        return "SOFT"
    return "OTHER"


def build_apt(conn, csv_zf):
    """Import airports with decimal coordinates."""
    import_csv(conn, "APT_BASE", csv_zf, "APT_BASE.csv", [
        "SITE_NO", "SITE_TYPE_CODE", "STATE_CODE", "ARPT_ID", "CITY",
        "COUNTRY_CODE", "ARPT_NAME", "OWNERSHIP_TYPE_CODE",
        "FACILITY_USE_CODE", "LAT_DECIMAL", "LONG_DECIMAL", "ELEV",
        "MAG_VARN", "MAG_HEMIS", "TPA",
        "RESP_ARTCC_ID", "FSS_ID", "NOTAM_ID",
        "ACTIVATION_DATE", "ARPT_STATUS",
        "FUEL_TYPES", "LGT_SKED", "BCN_LGT_SKED",
        "TWR_TYPE_CODE", "ICAO_ID",
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

    conn.execute("CREATE INDEX idx_apt_base_site_no ON APT_BASE(SITE_NO)")

    # Airport airspace class (B/C/D/E flags per airport)
    import_csv(conn, "CLS_ARSP", csv_zf, "CLS_ARSP.csv", [
        "SITE_NO", "CLASS_B_AIRSPACE", "CLASS_C_AIRSPACE",
        "CLASS_D_AIRSPACE", "CLASS_E_AIRSPACE",
    ])
    conn.execute("CREATE INDEX idx_cls_arsp_site_no ON CLS_ARSP(SITE_NO)")


def build_nav(conn, csv_zf):
    """Import navaids with decimal coordinates."""
    import_csv(conn, "NAV_BASE", csv_zf, "NAV_BASE.csv", [
        "NAV_ID", "NAV_TYPE", "STATE_CODE", "CITY", "COUNTRY_CODE",
        "NAV_STATUS", "NAME",
        "OPER_HOURS", "HIGH_ALT_ARTCC_ID", "LOW_ALT_ARTCC_ID",
        "LAT_DECIMAL", "LONG_DECIMAL", "ELEV",
        "MAG_VARN", "MAG_VARN_HEMIS",
        "FREQ", "CHAN", "PWR_OUTPUT",
        "SIMUL_VOICE_FLAG", "VOICE_CALL", "RESTRICTION_FLAG",
        "ALT_CODE", "LOW_NAV_ON_HIGH_CHART_FLAG",
    ])

    # Altitude-band classification for navaids.
    # ALT_CODE: H=high-only, VH=both, VL/L/T/empty=low-only.
    # LOW_NAV_ON_HIGH_CHART_FLAG=Y promotes a low navaid onto high charts.
    conn.execute("ALTER TABLE NAV_BASE ADD COLUMN IS_LOW_NAV INTEGER")
    conn.execute("ALTER TABLE NAV_BASE ADD COLUMN IS_HIGH_NAV INTEGER")
    conn.execute("""
        UPDATE NAV_BASE SET
            IS_LOW_NAV = CASE WHEN ALT_CODE = 'H' THEN 0 ELSE 1 END,
            IS_HIGH_NAV = CASE
                WHEN ALT_CODE IN ('H', 'VH') THEN 1
                WHEN LOW_NAV_ON_HIGH_CHART_FLAG = 'Y' THEN 1
                ELSE 0 END
    """)

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


def build_fix(conn, csv_zf):
    """Import fixes with decimal coordinates."""
    import_csv(conn, "FIX_BASE", csv_zf, "FIX_BASE.csv", [
        "FIX_ID", "STATE_CODE", "COUNTRY_CODE",
        "ICAO_REGION_CODE", "LAT_DECIMAL", "LONG_DECIMAL",
        "FIX_USE_CODE", "ARTCC_ID_HIGH", "ARTCC_ID_LOW", "CHARTS",
    ])

    # Altitude-band classification for fixes.
    # CHARTS is a comma-separated list like "ENROUTE HIGH,ENROUTE LOW,IAP".
    # A fix shown on any HIGH chart is high-relevant; anything not
    # exclusively high (terminal/procedure fixes on IAP/STAR/SID) is low.
    conn.execute("ALTER TABLE FIX_BASE ADD COLUMN IS_LOW_FIX INTEGER")
    conn.execute("ALTER TABLE FIX_BASE ADD COLUMN IS_HIGH_FIX INTEGER")
    conn.execute("""
        UPDATE FIX_BASE SET
            IS_HIGH_FIX = CASE WHEN CHARTS LIKE '%HIGH%' THEN 1 ELSE 0 END,
            IS_LOW_FIX = CASE
                WHEN CHARTS LIKE '%LOW%' THEN 1
                WHEN CHARTS LIKE '%HIGH%' THEN 0
                ELSE 1 END
    """)

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

    # Chart association: which charts each fix appears on
    import_csv(conn, "FIX_CHRT", csv_zf, "FIX_CHRT.csv", [
        "FIX_ID", "ICAO_REGION_CODE", "STATE_CODE", "COUNTRY_CODE",
        "CHARTING_TYPE_DESC",
    ])
    conn.execute("CREATE INDEX idx_fix_chrt ON FIX_CHRT(FIX_ID)")

    count = conn.execute("SELECT COUNT(*) FROM FIX_CHRT").fetchone()[0]
    print(f"  FIX_CHRT: {count} rows")


def build_awy(conn, csv_zf):
    """Import airways and build segment table with resolved coordinates.

    AWY_SEG_ALT references waypoints by name. We resolve FROM_POINT
    coordinates by joining against NAV_BASE and FIX_BASE.
    """
    import_csv(conn, "AWY_SEG_ALT", csv_zf, "AWY_SEG_ALT.csv", [
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
            s.MIN_ENROUTE_ALT,
            s.MAG_COURSE_DIST
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
               AWY_SEG_GAP_FLAG, MIN_ENROUTE_ALT, MAG_COURSE_DIST
        FROM AWY_SEG
        GROUP BY AWY_ID, AWY_LOCATION, POINT_SEQ
    """)
    conn.execute("DROP TABLE AWY_SEG")
    conn.execute("ALTER TABLE AWY_SEG_DEDUP RENAME TO AWY_SEG")

    # Duplicate segments that cross the antimeridian with adjusted longitudes
    crossing = conn.execute("""
        SELECT AWY_ID, AWY_LOCATION, POINT_SEQ, FROM_POINT, TO_POINT,
               FROM_LAT, FROM_LON, TO_LAT, TO_LON,
               AWY_SEG_GAP_FLAG, MIN_ENROUTE_ALT, MAG_COURSE_DIST
        FROM AWY_SEG
        WHERE ABS(FROM_LON - TO_LON) > 180
    """).fetchall()
    for row in crossing:
        from_lon, to_lon = row[6], row[8]
        # West copy: shift the eastern endpoint by -360
        if to_lon > from_lon:
            west = row[:6] + (from_lon, row[7], to_lon - 360) + row[9:]
            east = row[:6] + (from_lon + 360, row[7], to_lon) + row[9:]
        else:
            west = row[:6] + (from_lon - 360, row[7], to_lon) + row[9:]
            east = row[:6] + (from_lon, row[7], to_lon + 360) + row[9:]
        conn.execute("""
            INSERT INTO AWY_SEG VALUES (?,?,?,?,?,?,?,?,?,?,?,?)
        """, west)
        conn.execute("""
            INSERT INTO AWY_SEG VALUES (?,?,?,?,?,?,?,?,?,?,?,?)
        """, east)
    # Remove the original crossing segments
    conn.execute("DELETE FROM AWY_SEG WHERE ABS(FROM_LON - TO_LON) > 180")

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


def build_pja(conn, csv_zf):
    """Import parachute jump areas (point + radius)."""
    import_csv(conn, "PJA_RAW", csv_zf, "PJA_BASE.csv", [
        "PJA_ID", "DROP_ZONE_NAME", "LAT_DECIMAL", "LONG_DECIMAL",
        "PJA_RADIUS", "MAX_ALTITUDE",
    ])

    conn.execute("DROP TABLE IF EXISTS PJA_BASE")
    conn.execute("""
        CREATE TABLE PJA_BASE AS
        SELECT
            PJA_ID,
            DROP_ZONE_NAME AS NAME,
            CAST(LAT_DECIMAL AS REAL) AS LAT,
            CAST(LONG_DECIMAL AS REAL) AS LON,
            CASE WHEN PJA_RADIUS IS NOT NULL AND TRIM(PJA_RADIUS) != ''
                 THEN CAST(PJA_RADIUS AS REAL) ELSE 0.0 END AS RADIUS_NM,
            MAX_ALTITUDE,
            CASE WHEN MAX_ALTITUDE IS NOT NULL AND TRIM(MAX_ALTITUDE) != ''
                 THEN CAST(MAX_ALTITUDE AS INTEGER) ELSE 0 END AS MAX_ALT_FT_MSL
        FROM PJA_RAW
        WHERE LAT_DECIMAL IS NOT NULL AND LAT_DECIMAL != ''
          AND LONG_DECIMAL IS NOT NULL AND LONG_DECIMAL != ''
    """)
    conn.execute("DROP TABLE PJA_RAW")

    count = conn.execute("SELECT COUNT(*) FROM PJA_BASE").fetchone()[0]
    print(f"  PJA_BASE: {count} parachute jump areas")

    conn.execute("""
        CREATE VIRTUAL TABLE PJA_BASE_RTREE USING rtree(
            id, min_lon, max_lon, min_lat, max_lat
        )
    """)
    # Each PJA's R-tree bbox covers its full circle so pick queries with a
    # degenerate point bbox still match when the click lands inside the disc.
    pja_rows = conn.execute(
        "SELECT rowid, LON, LAT, RADIUS_NM FROM PJA_BASE").fetchall()
    conn.executemany(
        "INSERT INTO PJA_BASE_RTREE (id, min_lon, max_lon, min_lat, max_lat) "
        "VALUES (?, ?, ?, ?, ?)",
        [bbox_for_circle(*row) for row in pja_rows])


def build_mtr(conn, csv_zf):
    """Import military training routes and build segment table.

    MTR_PT contains route points with coordinates. We build segments
    from consecutive points within each route.
    """
    import_csv(conn, "MTR_PT", csv_zf, "MTR_PT.csv", [
        "ROUTE_TYPE_CODE", "ROUTE_ID", "ROUTE_PT_SEQ",
        "ROUTE_PT_ID", "NEXT_ROUTE_PT_ID",
        "LAT_DECIMAL", "LONG_DECIMAL",
        "NAV_ID",
    ])

    # Build segments by joining each point to the next point in sequence
    conn.execute("""
        CREATE TABLE MTR_SEG AS
        SELECT
            p1.ROUTE_TYPE_CODE || p1.ROUTE_ID AS MTR_ID,
            p1.ROUTE_TYPE_CODE AS ROUTE_TYPE_CODE,
            COALESCE(NULLIF(p1.NAV_ID, ''), p1.ROUTE_PT_ID) AS FROM_POINT,
            COALESCE(NULLIF(p2.NAV_ID, ''), p2.ROUTE_PT_ID) AS TO_POINT,
            CAST(p1.LAT_DECIMAL AS REAL) AS FROM_LAT,
            CAST(p1.LONG_DECIMAL AS REAL) AS FROM_LON,
            CAST(p2.LAT_DECIMAL AS REAL) AS TO_LAT,
            CAST(p2.LONG_DECIMAL AS REAL) AS TO_LON
        FROM MTR_PT p1
        JOIN MTR_PT p2
            ON p2.ROUTE_TYPE_CODE = p1.ROUTE_TYPE_CODE
            AND p2.ROUTE_ID = p1.ROUTE_ID
            AND p2.ROUTE_PT_ID = p1.NEXT_ROUTE_PT_ID
        WHERE p1.LAT_DECIMAL IS NOT NULL AND p1.LAT_DECIMAL != ''
          AND p1.LONG_DECIMAL IS NOT NULL AND p1.LONG_DECIMAL != ''
          AND p2.LAT_DECIMAL IS NOT NULL AND p2.LAT_DECIMAL != ''
          AND p2.LONG_DECIMAL IS NOT NULL AND p2.LONG_DECIMAL != ''
    """)

    # Drop the raw points table
    conn.execute("DROP TABLE MTR_PT")

    count = conn.execute("SELECT COUNT(*) FROM MTR_SEG").fetchone()[0]
    print(f"  MTR_SEG: {count} segments")

    # R-tree on segment bounding boxes
    conn.execute("""
        CREATE VIRTUAL TABLE MTR_SEG_RTREE USING rtree(
            id, min_lon, max_lon, min_lat, max_lat
        )
    """)
    conn.execute("""
        INSERT INTO MTR_SEG_RTREE (id, min_lon, max_lon, min_lat, max_lat)
        SELECT rowid,
               MIN(FROM_LON, TO_LON), MAX(FROM_LON, TO_LON),
               MIN(FROM_LAT, TO_LAT), MAX(FROM_LAT, TO_LAT)
        FROM MTR_SEG
    """)


def build_maa(conn, csv_zf):
    """Import miscellaneous activity areas.

    Three kinds:
    - Shape-defined: areas with polygon boundary in MAA_SHP (aerobatic practice)
    - Point+radius: center point with radius in NM (some aerobatic practice)
    - Point-only: center point, no radius (glider, hang glider, ultralight, space launch)

    MAA_BASE stores all areas with type, name, center coords, and radius.
    MAA_SHP stores polygon points for shape-defined areas.
    """
    import_csv(conn, "MAA_RAW", csv_zf, "MAA_BASE.csv", [
        "MAA_ID", "MAA_TYPE_NAME", "MAA_NAME",
        "LATITUDE", "LONGITUDE", "MAA_RADIUS",
        "MAX_ALT", "MIN_ALT",
    ])

    # Import shape points and convert DMS to decimal
    import_csv(conn, "MAA_SHP_RAW", csv_zf, "MAA_SHP.csv", [
        "MAA_ID", "POINT_SEQ", "LATITUDE", "LONGITUDE",
    ])

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

    cursor = conn.execute("SELECT rowid, LAT_DMS, LON_DMS FROM MAA_SHP")
    updates = []
    for row in cursor:
        lat = parse_dms(row[1])
        lon = parse_dms(row[2])
        if lat is not None and lon is not None:
            updates.append((lat, lon, row[0]))
    conn.executemany(
        "UPDATE MAA_SHP SET LAT_DECIMAL = ?, LON_DECIMAL = ? WHERE rowid = ?",
        updates)
    print(f"  MAA_SHP: {len(updates)} shape points converted")

    # Reorder shape points into convex hull winding order.
    # FAA data stores quad vertices in arbitrary order (often as opposite
    # corner pairs), causing bowtie rendering artifacts.
    rows = conn.execute(
        "SELECT MAA_ID, POINT_SEQ, LAT_DECIMAL, LON_DECIMAL FROM MAA_SHP "
        "WHERE LAT_DECIMAL != 0.0 ORDER BY MAA_ID, POINT_SEQ").fetchall()

    by_id = {}
    for maa_id, seq, lat, lon in rows:
        by_id.setdefault(maa_id, []).append((seq, lat, lon))

    reorder_updates = []
    for maa_id, pts in by_id.items():
        if len(pts) < 3:
            continue
        clat = sum(p[1] for p in pts) / len(pts)
        clon = sum(p[2] for p in pts) / len(pts)
        sorted_pts = sorted(pts, key=lambda p: math.atan2(p[1] - clat, p[2] - clon))
        for new_seq, (old_seq, lat, lon) in enumerate(sorted_pts, 1):
            if new_seq != old_seq:
                reorder_updates.append((new_seq, maa_id, old_seq))

    if reorder_updates:
        conn.executemany(
            "UPDATE MAA_SHP SET POINT_SEQ = -?1 WHERE MAA_ID = ?2 AND POINT_SEQ = ?3",
            reorder_updates)
        conn.execute("UPDATE MAA_SHP SET POINT_SEQ = -POINT_SEQ WHERE POINT_SEQ < 0")
        print(f"  MAA_SHP: reordered {len(reorder_updates)} points into convex hull order")

    conn.execute("CREATE INDEX idx_maa_shp ON MAA_SHP(MAA_ID)")

    # Build MAA_BASE with parsed coordinates
    conn.execute("""
        CREATE TABLE MAA_BASE AS
        SELECT
            MAA_ID,
            MAA_TYPE_NAME AS TYPE,
            MAA_NAME AS NAME,
            LATITUDE AS LAT_DMS,
            LONGITUDE AS LON_DMS,
            0.0 AS LAT,
            0.0 AS LON,
            CASE WHEN MAA_RADIUS IS NOT NULL AND TRIM(MAA_RADIUS) != ''
                 THEN CAST(MAA_RADIUS AS REAL) ELSE 0.0 END AS RADIUS_NM,
            MAX_ALT, MIN_ALT
        FROM MAA_RAW
    """)
    conn.execute("DROP TABLE MAA_RAW")

    cursor = conn.execute(
        "SELECT rowid, LAT_DMS, LON_DMS FROM MAA_BASE WHERE TRIM(LAT_DMS) != ''")
    updates = []
    for row in cursor:
        lat = parse_dms(row[1])
        lon = parse_dms(row[2])
        if lat is not None and lon is not None:
            updates.append((lat, lon, row[0]))
    conn.executemany(
        "UPDATE MAA_BASE SET LAT = ?, LON = ? WHERE rowid = ?", updates)

    # Drop DMS columns
    conn.execute("ALTER TABLE MAA_BASE DROP COLUMN LAT_DMS")
    conn.execute("ALTER TABLE MAA_BASE DROP COLUMN LON_DMS")

    total = conn.execute("SELECT COUNT(*) FROM MAA_BASE").fetchone()[0]
    pts = conn.execute("SELECT COUNT(*) FROM MAA_BASE WHERE LAT != 0.0").fetchone()[0]
    shps = total - pts
    print(f"  MAA_BASE: {total} areas ({pts} point/radius, {shps} shape-defined)")

    # R-tree: point-based use their coordinates, shape-based use MAA_SHP bbox
    conn.execute("""
        CREATE VIRTUAL TABLE MAA_BASE_RTREE USING rtree(
            id, min_lon, max_lon, min_lat, max_lat
        )
    """)
    maa_point_rows = conn.execute(
        "SELECT rowid, LON, LAT, RADIUS_NM FROM MAA_BASE WHERE LAT != 0.0"
    ).fetchall()
    conn.executemany(
        "INSERT INTO MAA_BASE_RTREE (id, min_lon, max_lon, min_lat, max_lat) "
        "VALUES (?, ?, ?, ?, ?)",
        [bbox_for_circle(*row) for row in maa_point_rows])
    conn.execute("""
        INSERT INTO MAA_BASE_RTREE (id, min_lon, max_lon, min_lat, max_lat)
        SELECT b.rowid,
               MIN(s.LON_DECIMAL), MAX(s.LON_DECIMAL),
               MIN(s.LAT_DECIMAL), MAX(s.LAT_DECIMAL)
        FROM MAA_BASE b
        JOIN MAA_SHP s ON s.MAA_ID = b.MAA_ID
        WHERE b.LAT = 0.0 AND s.LAT_DECIMAL != 0.0
        GROUP BY b.rowid
    """)


def build_apt_rwy(conn, csv_zf):
    """Import runway data for high-zoom rendering."""
    import_csv(conn, "APT_RWY", csv_zf, "APT_RWY.csv", [
        "SITE_NO", "RWY_ID", "RWY_LEN", "RWY_WIDTH", "SURFACE_TYPE_CODE",
        "COND", "RWY_LGT_CODE",
    ])

    import_csv(conn, "APT_RWY_END", csv_zf, "APT_RWY_END.csv", [
        "SITE_NO", "RWY_ID", "RWY_END_ID", "LAT_DECIMAL", "LONG_DECIMAL",
        "RWY_END_ELEV", "TRUE_ALIGNMENT",
        "ILS_TYPE", "RIGHT_HAND_TRAFFIC_PAT_FLAG",
        "RWY_MARKING_TYPE_CODE", "RWY_MARKING_COND",
        "VGSI_CODE", "APCH_LGT_SYSTEM_CODE",
        "RWY_END_LGTS_FLAG", "CNTRLN_LGTS_AVBL_FLAG", "TDZ_LGT_AVBL_FLAG",
        "DISPLACED_THR_LEN", "LAT_DISPLACED_THR_DECIMAL", "LONG_DISPLACED_THR_DECIMAL",
        "TKOF_RUN_AVBL", "TKOF_DIST_AVBL", "ACLT_STOP_DIST_AVBL", "LNDG_DIST_AVBL",
    ])

    conn.execute("CREATE INDEX idx_apt_rwy_end_site ON APT_RWY_END(SITE_NO, RWY_ID)")

    # Add surface classification column to APT_BASE.
    # For each airport, take the hardest surface across all runways
    # (HARD > SOFT > OTHER), plus the max runway length.
    conn.execute("ALTER TABLE APT_BASE ADD COLUMN HARD_SURFACE INTEGER DEFAULT 0")

    # Build per-airport surface classification and max runway length
    cursor = conn.execute("""
        SELECT a.rowid, r.SURFACE_TYPE_CODE, r.RWY_LEN
        FROM APT_BASE a
        JOIN APT_RWY r ON r.SITE_NO = a.SITE_NO
    """)

    # Accumulate hardest surface per airport rowid (HARD > SOFT > OTHER)
    airport_data = {}
    rank = {"HARD": 2, "SOFT": 1, "OTHER": 0}
    for rowid, surface_code, rwy_len in cursor:
        classification = classify_surface(surface_code.strip() if surface_code else "")
        if rowid not in airport_data:
            airport_data[rowid] = classification
        else:
            prev = airport_data[rowid]
            if rank[classification] > rank[prev]:
                airport_data[rowid] = classification

    updates = [(1 if cls == "HARD" else 0, rowid) for rowid, cls in airport_data.items()]
    conn.executemany(
        "UPDATE APT_BASE SET HARD_SURFACE = ? WHERE rowid = ?",
        updates,
    )

    counts = {}
    for cls in airport_data.values():
        counts[cls] = counts.get(cls, 0) + 1
    print(f"  Surface classification: {counts}")

    # Pre-build runway segments with resolved endpoint coordinates
    # and their own R-tree for direct spatial queries
    conn.execute("""
        CREATE TABLE RWY_SEG AS
        SELECT
            e1.SITE_NO,
            e1.RWY_ID,
            e1.LAT_DECIMAL AS END1_LAT,
            e1.LONG_DECIMAL AS END1_LON,
            e2.LAT_DECIMAL AS END2_LAT,
            e2.LONG_DECIMAL AS END2_LON
        FROM APT_RWY_END e1
        JOIN APT_RWY_END e2
            ON e1.SITE_NO = e2.SITE_NO AND e1.RWY_ID = e2.RWY_ID
        WHERE e1.rowid < e2.rowid
            AND e1.LAT_DECIMAL != '' AND e1.LONG_DECIMAL != ''
            AND e2.LAT_DECIMAL != '' AND e2.LONG_DECIMAL != ''
    """)

    count = conn.execute("SELECT COUNT(*) FROM RWY_SEG").fetchone()[0]
    print(f"  RWY_SEG: {count} runway segments")

    conn.execute("""
        CREATE VIRTUAL TABLE RWY_SEG_RTREE USING rtree(
            id, min_lon, max_lon, min_lat, max_lat
        )
    """)
    conn.execute("""
        INSERT INTO RWY_SEG_RTREE (id, min_lon, max_lon, min_lat, max_lat)
        SELECT rowid,
               MIN(CAST(END1_LON AS REAL), CAST(END2_LON AS REAL)),
               MAX(CAST(END1_LON AS REAL), CAST(END2_LON AS REAL)),
               MIN(CAST(END1_LAT AS REAL), CAST(END2_LAT AS REAL)),
               MAX(CAST(END1_LAT AS REAL), CAST(END2_LAT AS REAL))
        FROM RWY_SEG
    """)


def build_apt_att(conn, csv_zf):
    """Import airport attendance schedules."""
    import_csv(conn, "APT_ATT", csv_zf, "APT_ATT.csv", [
        "SITE_NO", "SKED_SEQ_NO", "MONTH", "DAY", "HOUR",
    ])
    conn.execute("CREATE INDEX idx_apt_att_site ON APT_ATT(SITE_NO)")


def build_apt_rmk(conn, csv_zf):
    """Import airport remarks."""
    import_csv(conn, "APT_RMK", csv_zf, "APT_RMK.csv", [
        "SITE_NO", "TAB_NAME", "REF_COL_NAME", "ELEMENT",
        "REF_COL_SEQ_NO", "REMARK",
    ])
    conn.execute("CREATE INDEX idx_apt_rmk_site ON APT_RMK(SITE_NO)")


def build_ils(conn, csv_zf):
    """Import instrument landing system data."""
    import_csv(conn, "ILS_BASE", csv_zf, "ILS_BASE.csv", [
        "SITE_NO", "ARPT_ID", "RWY_END_ID", "ILS_LOC_ID",
        "SYSTEM_TYPE_CODE", "CATEGORY",
        "LAT_DECIMAL", "LONG_DECIMAL", "SITE_ELEVATION",
        "LOC_FREQ", "APCH_BEAR",
        "COMPONENT_STATUS", "BK_COURSE_STATUS_CODE",
    ])
    conn.execute("CREATE INDEX idx_ils_base_site ON ILS_BASE(SITE_NO)")

    import_csv(conn, "ILS_GS", csv_zf, "ILS_GS.csv", [
        "SITE_NO", "RWY_END_ID", "ILS_LOC_ID",
        "LAT_DECIMAL", "LONG_DECIMAL", "SITE_ELEVATION",
        "G_S_TYPE_CODE", "G_S_ANGLE", "G_S_FREQ",
        "COMPONENT_STATUS",
    ])
    conn.execute("CREATE INDEX idx_ils_gs_site ON ILS_GS(SITE_NO)")

    import_csv(conn, "ILS_DME", csv_zf, "ILS_DME.csv", [
        "SITE_NO", "RWY_END_ID", "ILS_LOC_ID",
        "LAT_DECIMAL", "LONG_DECIMAL", "SITE_ELEVATION",
        "CHANNEL", "COMPONENT_STATUS",
    ])
    conn.execute("CREATE INDEX idx_ils_dme_site ON ILS_DME(SITE_NO)")

    import_csv(conn, "ILS_MKR", csv_zf, "ILS_MKR.csv", [
        "SITE_NO", "RWY_END_ID", "ILS_LOC_ID",
        "ILS_COMP_TYPE_CODE", "COMPONENT_STATUS",
        "LAT_DECIMAL", "LONG_DECIMAL", "SITE_ELEVATION",
        "MKR_FAC_TYPE_CODE", "MARKER_ID_BEACON",
        "COMPASS_LOCATOR_NAME", "FREQ",
        "NAV_ID", "NAV_TYPE", "LOW_POWERED_NDB_STATUS",
    ])
    conn.execute("CREATE INDEX idx_ils_mkr_site ON ILS_MKR(SITE_NO)")

    import_csv(conn, "ILS_RMK", csv_zf, "ILS_RMK.csv", [
        "SITE_NO", "RWY_END_ID", "ILS_LOC_ID",
        "ILS_COMP_TYPE_CODE", "TAB_NAME", "REF_COL_NAME",
        "REF_COL_SEQ_NO", "REMARK",
    ])
    conn.execute("CREATE INDEX idx_ils_rmk_site ON ILS_RMK(SITE_NO)")


def build_atc(conn, csv_zf):
    """Import ATC facility data."""
    import_csv(conn, "ATC_BASE", csv_zf, "ATC_BASE.csv", [
        "SITE_NO", "FACILITY_TYPE", "FACILITY_ID",
        "CITY", "STATE_CODE", "COUNTRY_CODE",
        "ICAO_ID", "FACILITY_NAME",
        "TWR_OPERATOR_CODE", "TWR_CALL", "TWR_HRS",
        "PRIMARY_APCH_RADIO_CALL", "SECONDARY_APCH_RADIO_CALL",
        "PRIMARY_DEP_RADIO_CALL", "SECONDARY_DEP_RADIO_CALL",
    ])
    conn.execute("CREATE INDEX idx_atc_base_site ON ATC_BASE(SITE_NO)")
    conn.execute("CREATE INDEX idx_atc_base_fac ON ATC_BASE(FACILITY_ID, FACILITY_TYPE)")

    import_csv(conn, "ATC_ATIS", csv_zf, "ATC_ATIS.csv", [
        "FACILITY_ID", "FACILITY_TYPE",
        "ATIS_NO", "DESCRIPTION", "ATIS_HRS", "ATIS_PHONE_NO",
    ])
    conn.execute("CREATE INDEX idx_atc_atis_fac ON ATC_ATIS(FACILITY_ID, FACILITY_TYPE)")

    import_csv(conn, "ATC_RMK", csv_zf, "ATC_RMK.csv", [
        "FACILITY_ID", "FACILITY_TYPE",
        "TAB_NAME", "REF_COL_NAME", "REMARK_NO", "REMARK",
    ])
    conn.execute("CREATE INDEX idx_atc_rmk_fac ON ATC_RMK(FACILITY_ID, FACILITY_TYPE)")

    import_csv(conn, "ATC_SVC", csv_zf, "ATC_SVC.csv", [
        "FACILITY_ID", "FACILITY_TYPE", "CTL_SVC",
    ])
    conn.execute("CREATE INDEX idx_atc_svc_fac ON ATC_SVC(FACILITY_ID, FACILITY_TYPE)")


def simplify_ring(points, epsilon):
    """Simplify a polygon ring using Shapely's GEOS-backed RDP.

    Points are (lon, lat) tuples. Epsilon is in degrees.
    """
    if len(points) <= 3:
        return points
    from shapely.geometry import LineString
    simplified = LineString(points).simplify(epsilon, preserve_topology=False)
    return list(simplified.coords)


def read_dbf(f):
    """Read a .dbf file, yielding dicts of field_name -> string value.

    f is a binary file-like object.
    """
    f.read(1)  # version
    f.read(3)  # date
    nrec = struct.unpack("<I", f.read(4))[0]
    hdr_size = struct.unpack("<H", f.read(2))[0]
    rec_size = struct.unpack("<H", f.read(2))[0]
    f.read(20)  # reserved

    fields = []
    while True:
        peek = f.read(1)
        if peek == b"\r":
            break
        name = (peek + f.read(10)).rstrip(b"\x00").decode("ascii")
        ftype = f.read(1).decode("ascii")
        f.read(4)  # reserved
        flen = struct.unpack("B", f.read(1))[0]
        f.read(15)  # reserved
        fields.append((name, flen))

    f.seek(hdr_size)
    for _ in range(nrec):
        f.read(1)  # deletion flag
        record = {}
        for name, flen in fields:
            val = f.read(flen).decode("latin-1").strip()
            record[name] = val
        yield record


def signed_ring_area(ring):
    """Compute signed area of a polygon ring using the shoelace formula.

    Negative = clockwise (outer ring in ESRI shapefile convention).
    Positive = counterclockwise (hole in ESRI shapefile convention).
    """
    area = 0.0
    n = len(ring)
    for i in range(n):
        x0, y0 = ring[i]
        x1, y1 = ring[(i + 1) % n]
        area += x0 * y1 - x1 * y0
    return area / 2.0


def read_shp_polygons(f):
    """Read polygon geometries from a .shp file.

    f is a binary file-like object.
    Yields lists of (ring, is_hole) tuples, where each ring is a list of
    (lon, lat) tuples and is_hole indicates clockwise winding (hole).
    Handles both Polygon (type 5) and PolygonZ (type 15).
    """
    # File header: 100 bytes
    f.read(100)

    while True:
        # Record header: 8 bytes (record number + content length in 16-bit words)
        rec_hdr = f.read(8)
        if len(rec_hdr) < 8:
            break
        content_length = struct.unpack(">I", rec_hdr[4:8])[0] * 2
        rec_data = f.read(content_length)
        if len(rec_data) < content_length:
            break

        shape_type = struct.unpack("<I", rec_data[0:4])[0]
        if shape_type == 0:
            yield []
            continue

        # Polygon (5) and PolygonZ (15) share the same base layout
        # bbox: 4 doubles, num_parts: int, num_points: int
        num_parts = struct.unpack("<I", rec_data[36:40])[0]
        num_points = struct.unpack("<I", rec_data[40:44])[0]

        # Part indices
        parts_offset = 44
        part_indices = list(struct.unpack(
            f"<{num_parts}I",
            rec_data[parts_offset:parts_offset + 4 * num_parts],
        ))
        part_indices.append(num_points)

        # Points (x=lon, y=lat pairs)
        points_offset = parts_offset + 4 * num_parts
        all_points = struct.unpack(
            f"<{num_points * 2}d",
            rec_data[points_offset:points_offset + 16 * num_points],
        )

        parts = []
        for i in range(num_parts):
            start = part_indices[i]
            end = part_indices[i + 1]
            # Slice the flat doubles array: [x0,y0,x1,y1,...] -> [(x0,y0),...]
            flat = all_points[start * 2:end * 2]
            ring = list(zip(flat[0::2], flat[1::2]))
            is_hole = signed_ring_area(ring) > 0
            parts.append((ring, is_hole))

        yield parts


# WGS84 mean radius, used for great-circle distance/destination math.
EARTH_RADIUS_M = 6371008.8


def _radius_to_meters(radius, uom):
    if uom == "NM":
        return radius * 1852.0
    if uom == "MI":
        return radius * 1609.344
    if uom == "FT":
        return radius * 0.3048
    if uom == "KM":
        return radius * 1000.0
    if uom == "M":
        return radius
    return radius * 1852.0  # default to NM


def destination_point(lat_deg, lon_deg, bearing_deg, distance_m):
    """Great-circle destination from origin.

    Bearing is degrees clockwise from true north. Returns (lon, lat) in
    degrees. Uses the spherical earth model with WGS84 mean radius.
    """
    d_R = distance_m / EARTH_RADIUS_M
    lat1 = math.radians(lat_deg)
    lon1 = math.radians(lon_deg)
    br = math.radians(bearing_deg)
    sin_lat2 = (math.sin(lat1) * math.cos(d_R)
                + math.cos(lat1) * math.sin(d_R) * math.cos(br))
    lat2 = math.asin(sin_lat2)
    lon2 = lon1 + math.atan2(
        math.sin(br) * math.sin(d_R) * math.cos(lat1),
        math.cos(d_R) - math.sin(lat1) * sin_lat2)
    return math.degrees(lon2), math.degrees(lat2)


def _math_angle_to_bearing(math_deg):
    # Current FAA AIXM-SAA data encodes arc angles in math convention:
    # 0° = east, 90° = north, CCW. Convert to compass bearing (0° = north, CW).
    return (90.0 - math_deg) % 360.0


def generate_circle(center_lon, center_lat, radius, uom, num_points=72):
    """Polygon approximating a circle of constant great-circle radius."""
    meters = _radius_to_meters(radius, uom)
    points = []
    for i in range(num_points):
        math_deg = 360.0 * i / num_points
        bearing = _math_angle_to_bearing(math_deg)
        points.append(destination_point(center_lat, center_lon, bearing, meters))
    points.append(points[0])
    return points


def generate_arc(center_lon, center_lat, radius, uom, start_deg, end_deg):
    """Points along an arc from start_deg to end_deg.

    Angles are degrees in math convention (0° = east, CCW), matching the
    arc-angle encoding seen in the FAA AIXM-SAA source. Each sample uses
    great-circle destination math so the distance is a true great-circle
    radius (not a planar offset that grows worse at high latitudes).
    """
    meters = _radius_to_meters(radius, uom)
    sweep = end_deg - start_deg
    num_points = max(int(abs(sweep) / 5.0), 2)
    points = []
    for i in range(num_points + 1):
        math_deg = start_deg + sweep * i / num_points
        bearing = _math_angle_to_bearing(math_deg)
        points.append(destination_point(center_lat, center_lon, bearing, meters))
    return points



GML = "http://www.opengis.net/gml/3.2"
AIXM = "http://www.aixm.aero/schema/5.0"
SAA = "urn:us:gov:dot:faa:aim:saa"
SUA_NS = "urn:us:gov:dot:faa:aim:saa:sua"


def parse_ring_points(ring_elem):
    """Extract polygon points from a GML Ring element.

    Handles LineStringSegment, CircleByCenterPoint, and ArcByCenterPoint
    curve segments. Multiple curveMember elements within a Ring are
    concatenated to form the complete boundary.

    Arc endpoints are snapped to the declared corner coordinates from
    adjacent LineStringSegments: those <gml:pos> values are the AIXM's
    authoritative corner positions, while an arc's mathematical endpoint
    (center + radius * angle) is an approximation that drifts tens to
    thousands of meters from the declared corner due to DMS rounding and
    spherical geometry. Without this, arc-to-line joins introduce
    self-intersections, spurious spikes at corners, and floating-point
    sliver artifacts downstream.
    """
    # First pass: collect segments as descriptors; detect pure circle.
    raw = []  # ("line", [(lon,lat),...]) | ("arc", clon, clat, rad, uom, s, e)
    for curve_member in ring_elem.findall(f"{{{GML}}}curveMember"):
        curve = curve_member.find(f"{{{GML}}}Curve")
        if curve is None:
            continue
        segments = curve.find(f"{{{GML}}}segments")
        if segments is None:
            continue
        for seg in segments:
            tag = seg.tag.split("}")[-1]
            if tag == "LineStringSegment":
                pts = []
                for pos in seg.findall(f"{{{GML}}}pos"):
                    lon, lat = pos.text.strip().split()
                    pts.append((float(lon), float(lat)))
                if pts:
                    raw.append(("line", pts))
            elif tag == "CircleByCenterPoint":
                pos = seg.find(f".//{{{GML}}}pos")
                rad = seg.find(f"{{{GML}}}radius")
                if pos is not None and rad is not None:
                    lon, lat = pos.text.strip().split()
                    clon, clat = float(lon), float(lat)
                    radius_val = float(rad.text)
                    uom = rad.get("uom", "NM")
                    circle_pts = generate_circle(clon, clat, radius_val, uom)
                    return circle_pts, {
                        "lon": clon, "lat": clat,
                        "radius": radius_val, "uom": uom,
                    }
            elif tag == "ArcByCenterPoint":
                pos = seg.find(f".//{{{GML}}}pos")
                rad = seg.find(f"{{{GML}}}radius")
                sa = seg.find(f"{{{GML}}}startAngle")
                ea = seg.find(f"{{{GML}}}endAngle")
                if (pos is not None and rad is not None
                        and sa is not None and ea is not None):
                    lon, lat = pos.text.strip().split()
                    raw.append(("arc", float(lon), float(lat),
                                float(rad.text), rad.get("uom", "NM"),
                                float(sa.text), float(ea.text)))

    n = len(raw)

    def prev_corner(i):
        # Only snap to an immediately adjacent LineStringSegment (treating
        # the ring as a closed loop). For arc-to-arc transitions we leave
        # the computed position alone — snapping to a distant line's
        # endpoint would teleport the arc across the ring.
        prev = raw[(i - 1) % n]
        return prev[1][-1] if prev[0] == "line" else None

    def next_corner(i):
        nxt = raw[(i + 1) % n]
        return nxt[1][0] if nxt[0] == "line" else None

    # Second pass: emit points, snapping arc endpoints to declared corners.
    points = []
    for i, seg in enumerate(raw):
        if seg[0] == "line":
            for p in seg[1]:
                if not points or points[-1] != p:
                    points.append(p)
        else:
            _, clon, clat, radius, uom, start_deg, end_deg = seg
            arc_pts = generate_arc(clon, clat, radius, uom, start_deg, end_deg)
            if not arc_pts:
                continue
            pc = prev_corner(i)
            nc = next_corner(i)
            if pc is not None:
                arc_pts[0] = pc
            if nc is not None:
                arc_pts[-1] = nc
            if points and points[-1] == arc_pts[0]:
                arc_pts = arc_pts[1:]
            points.extend(arc_pts)

    return points, None


def _text(parent, tag):
    """Get text content of a child element, or empty string."""
    elem = parent.find(f".//{tag}")
    return elem.text.strip() if elem is not None and elem.text else ""


def _collect_additive_shapes(airspace):
    """Extract BASE + UNION ring point lists from an AIXM Airspace.

    Returns a list of (shape, upper_str, lower_str) for each additive
    geometry component with a resolvable horizontal projection. Used
    by the SUBTR-by-reference path when R-4812 et al. express the
    carved-out region as `<contributorAirspace xlink:href=#...>`.
    """
    ts = airspace.find(f".//{{{AIXM}}}AirspaceTimeSlice")
    if ts is None:
        return []
    out = []
    for gc in ts.findall(f"{{{AIXM}}}geometryComponent"):
        agc = gc.find(f"{{{AIXM}}}AirspaceGeometryComponent")
        if agc is None:
            continue
        op = agc.find(f"{{{AIXM}}}operation")
        if op is None or op.text not in ("BASE", "UNION"):
            continue
        vol = agc.find(f".//{{{AIXM}}}AirspaceVolume")
        if vol is None:
            continue
        upper = vol.find(f"{{{AIXM}}}upperLimit")
        lower = vol.find(f"{{{AIXM}}}lowerLimit")
        uref = vol.find(f"{{{AIXM}}}upperLimitReference")
        lref = vol.find(f"{{{AIXM}}}lowerLimitReference")
        cu = (f"{upper.text} {upper.get('uom','')} "
              f"{uref.text if uref is not None else ''}".strip()
              if upper is not None and upper.text else "")
        cl = (f"{lower.text} {lower.get('uom','')} "
              f"{lref.text if lref is not None else ''}".strip()
              if lower is not None and lower.text else "")
        hp = vol.find(f".//{{{AIXM}}}horizontalProjection")
        if hp is None:
            continue
        shape = []
        ring = hp.find(f".//{{{GML}}}Ring")
        if ring is not None:
            shape, _ = parse_ring_points(ring)
        else:
            lr = hp.find(f".//{{{GML}}}LinearRing")
            if lr is not None:
                for pos in lr.findall(f"{{{GML}}}pos"):
                    if pos.text is None:
                        continue
                    lon, lat = pos.text.strip().split()
                    shape.append((float(lon), float(lat)))
        if shape:
            out.append((shape, cu, cl))
    return out


def _parse_one_airspace(airspace, airspace_lookup=None):
    """Parse a single AIXM Airspace element.

    Returns a dict with designator, name, sua_type, metadata fields,
    and parts, or None if the element has no usable geometry.

    `airspace_lookup` maps gml:id → Airspace element and is used to
    resolve cross-airspace SUBTR references (`<contributorAirspace>
    <theAirspace xlink:href="#..."/></contributorAirspace>`).
    """
    if airspace_lookup is None:
        airspace_lookup = {}
    ts = airspace.find(f".//{{{AIXM}}}AirspaceTimeSlice")
    if ts is None:
        return None

    desig_elem = ts.find(f"{{{AIXM}}}designator")
    name_elem = ts.find(f"{{{AIXM}}}name")
    designator = desig_elem.text if desig_elem is not None else ""
    name = name_elem.text if name_elem is not None else ""

    # Get SUA type from extension
    sua_type = ""
    for ext in ts.iter(f"{{{SUA_NS}}}suaType"):
        sua_type = ext.text or ""
        break

    # SAA extension metadata (administrativeArea, city, legalDefinitionType)
    admin_area = _text(ts, f"{{{SAA}}}administrativeArea")
    saa_city = _text(ts, f"{{{SAA}}}city")

    # AIXM metadata from AirspaceActivation and related elements
    activity = _text(ts, f"{{{AIXM}}}activity")
    status = _text(ts, f"{{{AIXM}}}statusActivation")
    working_hours = _text(ts, f"{{{AIXM}}}workingHours")
    military = _text(ts, f"{{{AIXM}}}military")
    compliant_icao = _text(ts, f"{{{AIXM}}}compliantICAO")

    # Legal definition note (annotation where propertyName=legalDefinitionType)
    legal_note = ""
    for annotation in ts.findall(f"{{{AIXM}}}annotation"):
        note_elem = annotation.find(f".//{{{AIXM}}}Note")
        if note_elem is None:
            continue
        prop = note_elem.find(f"{{{AIXM}}}propertyName")
        if prop is not None and prop.text == "legalDefinitionType":
            ln = note_elem.find(f".//{{{AIXM}}}note")
            if ln is not None and ln.text:
                legal_note = ln.text.strip()
                break

    # Collect geometry from all components, ordered by sequence
    upper_limit = ""
    lower_limit = ""
    min_alt_limit = ""
    max_alt_limit = ""
    additive = []   # BASE + UNION components (merged into outer rings)
    subtractive = []  # SUBTR components (altitude restriction zones)
    for gc in ts.findall(f"{{{AIXM}}}geometryComponent"):
        agc = gc.find(f"{{{AIXM}}}AirspaceGeometryComponent")
        if agc is None:
            continue
        op = agc.find(f"{{{AIXM}}}operation")
        if op is None or op.text not in ("BASE", "UNION", "SUBTR"):
            continue
        seq = agc.find(f"{{{AIXM}}}operationSequence")
        seq_num = int(seq.text) if seq is not None and seq.text else 0

        vol = agc.find(f".//{{{AIXM}}}AirspaceVolume")

        # Extract altitude limits from this component
        comp_upper = ""
        comp_lower = ""
        if vol is not None:
            upper = vol.find(f"{{{AIXM}}}upperLimit")
            lower = vol.find(f"{{{AIXM}}}lowerLimit")
            upper_ref = vol.find(f"{{{AIXM}}}upperLimitReference")
            lower_ref = vol.find(f"{{{AIXM}}}lowerLimitReference")
            if upper is not None:
                uom = upper.get("uom", "")
                ref = upper_ref.text if upper_ref is not None else ""
                comp_upper = f"{upper.text} {uom} {ref}".strip()
            if lower is not None:
                uom = lower.get("uom", "")
                ref = lower_ref.text if lower_ref is not None else ""
                comp_lower = f"{lower.text} {uom} {ref}".strip()

        if op.text == "BASE":
            upper_limit = comp_upper
            lower_limit = comp_lower
            # AIXM lets the BASE volume carry minimumLimit /
            # maximumLimit pair — terrain-clearance constraints
            # supplementing the MSL band (typical pattern: MOA
            # declared as "11000 MSL → FL180" with min 3000 SFC,
            # meaning ops must remain ≥ 3000 ft AGL inside that
            # MSL slab). Used by ~15 alpine SUAs in the corpus.
            min_lim_e = vol.find(f"{{{AIXM}}}minimumLimit") if vol is not None else None
            max_lim_e = vol.find(f"{{{AIXM}}}maximumLimit") if vol is not None else None
            min_ref_e = vol.find(f"{{{AIXM}}}minimumLimitReference") if vol is not None else None
            max_ref_e = vol.find(f"{{{AIXM}}}maximumLimitReference") if vol is not None else None
            if min_lim_e is not None and min_lim_e.text:
                min_uom = min_lim_e.get("uom", "")
                min_ref = min_ref_e.text if min_ref_e is not None else ""
                min_alt_limit = f"{min_lim_e.text} {min_uom} {min_ref}".strip()
            else:
                min_alt_limit = ""
            if max_lim_e is not None and max_lim_e.text:
                max_uom = max_lim_e.get("uom", "")
                max_ref = max_ref_e.text if max_ref_e is not None else ""
                max_alt_limit = f"{max_lim_e.text} {max_uom} {max_ref}".strip()
            else:
                max_alt_limit = ""

        shape = []
        circle_info = None
        hp = vol.find(f".//{{{AIXM}}}horizontalProjection") if vol is not None else None
        if hp is not None:
            ring = hp.find(f".//{{{GML}}}Ring")
            if ring is not None:
                shape, circle_info = parse_ring_points(ring)
            else:
                linear_ring = hp.find(f".//{{{GML}}}LinearRing")
                if linear_ring is not None:
                    for pos in linear_ring.findall(f"{{{GML}}}pos"):
                        if pos.text is None:
                            continue
                        lon, lat = pos.text.strip().split()
                        shape.append((float(lon), float(lat)))

        # SUBTR-by-reference: AIXM lets a SUBTR define its region by
        # pointing at another airspace in the same file rather than
        # listing a polygon. Resolve the xlink and emit one SUBTR per
        # additive component of the referenced airspace. Used by
        # R-4812 to subtract R-4804A and R-4810.
        if (op.text == "SUBTR" and not shape and vol is not None
                and airspace_lookup):
            ref_elem = vol.find(
                f".//{{{AIXM}}}contributorAirspace/"
                f"{{{AIXM}}}AirspaceVolumeDependency/"
                f"{{{AIXM}}}theAirspace")
            if ref_elem is not None:
                href = ref_elem.get(f"{{{XLINK}}}href", "")
                ref_id = href.lstrip("#")
                ref_airspace = airspace_lookup.get(ref_id)
                if ref_airspace is not None:
                    for ref_shape, ref_up, ref_lo in _collect_additive_shapes(
                            ref_airspace):
                        # Blank altitudes at the reference mean "inherit
                        # from the referenced airspace", which in practice
                        # always covers BASE's band for these cutouts.
                        eff_upper = comp_upper or ref_up
                        eff_lower = comp_lower or ref_lo
                        subtractive.append((seq_num, ref_shape,
                                            eff_upper, eff_lower))
                    # referenced-airspace SUBTRs handled; skip fallthrough
                    continue
        if shape:
            if op.text == "SUBTR":
                subtractive.append((seq_num, shape, comp_upper, comp_lower))
            else:
                additive.append((seq_num, shape, circle_info,
                                 comp_upper, comp_lower))

    if not additive:
        return None

    additive.sort(key=lambda c: c[0])

    # Partition additive components (BASE + UNIONs) by altitude band. A
    # UNION whose floor/ceiling differ from BASE (e.g. W-497B seq 3 at
    # 5000–UNL while BASE is GND–UNL) is a separate altitude stratum and
    # must not be geometrically merged with BASE — doing so loses the
    # altitude restriction AND creates bogus topology where the strata
    # don't share ring vertices. Components sharing an altitude band are
    # still unioned together (disjoint islands, edge-adjacent annex lobes).
    from shapely.geometry import Polygon as ShapelyPolygon
    from shapely.geometry import MultiPolygon as ShapelyMultiPolygon
    from shapely.ops import unary_union
    from shapely.validation import make_valid
    from shapely import set_precision
    # Snap to ~1 m grid so polygons that share edges (sampled with slightly
    # different floating-point vertices) produce a clean union instead of
    # sliver holes. Airspace boundaries are not defined to sub-meter
    # precision, so this is well below any meaningful threshold.
    SNAP = 1e-5
    # Drop outer polygon pieces below this absolute area in deg² —
    # 1e-5 ≈ 0.03 NM². Empirically, floating-point fragments from
    # `unary_union` on overlapping sampled circles fall in 1e-8…5e-6;
    # the smallest real disconnected airspace component in the corpus
    # (WARRIOR 1 LOW's eastern protrusion severed by a SUBTR) is
    # ~3.9e-4, well above. A prior ratio-based filter (< 0.1% of the
    # main piece) wrongly dropped that legitimate region.
    OUTER_SLIVER_AREA_MIN = 1e-5

    # Altitude bands are now (value, ref) tuples for each of (lower,
    # upper). Two bands are equal only when both value AND reference
    # match — otherwise we have no way to compare them without a DEM,
    # so they stay as distinct strata.
    base_lo_alt = parse_altitude(lower_limit)   # (value, ref)
    base_up_alt = parse_altitude(upper_limit)
    base_band = (base_lo_alt, base_up_alt)

    # Group by full (lower, upper) altitude band — same value AND
    # same reference. BASE's band comes first because BASE is seq 0.
    bands = {}  # band_key -> list of entries
    band_order = []
    for entry in additive:
        _, _, _, comp_up, comp_lo = entry
        key = (parse_altitude(comp_lo), parse_altitude(comp_up))
        if key not in bands:
            bands[key] = []
            band_order.append(key)
        bands[key].append(entry)

    def _union_band(entries):
        """Union and clean a set of same-altitude polygons.

        Returns (polys, raw_polys). polys is the flat list of valid
        Shapely Polygons after union+sliver-cleanup; raw_polys is the
        unchanged set_precision'd input polygons (needed for SUBTR
        clipping against the full BASE-band union).
        """
        raw = []
        for _, shape, _, _, _ in entries:
            if len(shape) < 3:
                continue
            p = make_valid(ShapelyPolygon(shape))
            if p.is_empty:
                continue
            p = set_precision(p, SNAP)
            if not p.is_empty:
                raw.append(p)
        if not raw:
            return [], []
        unioned = unary_union(raw) if len(raw) > 1 else raw[0]
        flat = []
        def _collect(g):
            if isinstance(g, ShapelyPolygon):
                if not g.is_empty:
                    flat.append(g)
            elif isinstance(g, ShapelyMultiPolygon):
                for sub in g.geoms:
                    _collect(sub)
            elif hasattr(g, "geoms"):
                for sub in g.geoms:
                    _collect(sub)
        _collect(unioned)
        return flat, raw

    # Classify each SUBTR by its altitude band relative to BASE:
    #
    #   full_cover  — SUBTR's lower≤BASE.lower AND upper≥BASE.upper, AND
    #                 both lower-ref and upper-ref match BASE's. Becomes
    #                 a true 2D hole in BASE.
    #   partial_cover — refs match BASE's on both sides but the band is
    #                 a proper subset. Goes through face-overlay logic
    #                 to produce active-slice strata.
    #   standalone — refs do not all match BASE's. Without a DEM we
    #                cannot compare an AGL value to an MSL value, so
    #                we emit the SUBTR as its own stratum (footprint
    #                clipped to BASE outline) without any merging,
    #                subset elimination, or active-slice math.
    base_lo_val, base_lo_ref = base_lo_alt
    base_up_val, base_up_ref = base_up_alt
    full_cover_subs = []
    partial_cover_subs = []
    standalone_subs = []
    for sub in subtractive:
        _, shape, sub_upper, sub_lower = sub
        sub_lo = parse_altitude(sub_lower)
        sub_up = parse_altitude(sub_upper)
        sub_lo_val, sub_lo_ref = sub_lo
        sub_up_val, sub_up_ref = sub_up
        refs_match = (sub_lo_ref == base_lo_ref and sub_up_ref == base_up_ref
                      and sub_lo_ref is not None and sub_up_ref is not None)
        if not refs_match:
            standalone_subs.append(sub)
            continue
        is_full = (
            sub_lo_val is not None and sub_up_val is not None
            and base_lo_val is not None and base_up_val is not None
            and sub_lo_val <= base_lo_val and sub_up_val >= base_up_val)
        (full_cover_subs if is_full else partial_cover_subs).append(sub)

    full_cover_hole_geom = None
    if full_cover_subs:
        sub_polys = []
        for _, shape, _, _ in full_cover_subs:
            if len(shape) < 3:
                continue
            sp = make_valid(ShapelyPolygon(shape))
            if not sp.is_empty:
                sub_polys.append(sp)
        if sub_polys:
            full_cover_hole_geom = unary_union(sub_polys) if len(sub_polys) > 1 else sub_polys[0]

    # Pre-compute raw BASE-band union so partial-cover SUBTRs can be
    # clipped against it before the main band loop runs.
    base_entries_pre = bands.get(base_band, [])
    base_raw_polys = []
    for _, shape, _, _, _ in base_entries_pre:
        if len(shape) < 3:
            continue
        p = make_valid(ShapelyPolygon(shape))
        if p.is_empty:
            continue
        p = set_precision(p, SNAP)
        if not p.is_empty:
            base_raw_polys.append(p)
    outer_union = None
    if base_raw_polys:
        outer_union = (unary_union(base_raw_polys) if len(base_raw_polys) > 1
                        else base_raw_polys[0])

    # Clip every partial-cover SUBTR to the BASE outline (minus
    # full-cover holes) and keep its altitude band. These are fed
    # through a full 2D face overlay after the band loop so the
    # emitted strata are 3D-disjoint: at each point in the plane, the
    # carved altitude range is the union of every overlapping SUBTR's
    # range, and active slices are the complement within BASE's band.
    partial_subs_clipped = []   # [{lo_ft, up_ft, sub_upper, sub_lower, poly}]
    partial_cover_union = None
    if partial_cover_subs and outer_union is not None:
        effective_outer = outer_union
        if full_cover_hole_geom is not None:
            effective_outer = effective_outer.difference(full_cover_hole_geom)
        partial_cover_subs.sort(key=lambda c: c[0])
        for _, shape, sub_upper, sub_lower in partial_cover_subs:
            if len(shape) < 3 or effective_outer.is_empty:
                continue
            sub_poly = make_valid(ShapelyPolygon(shape))
            if sub_poly.is_empty:
                continue
            clipped = sub_poly.intersection(effective_outer)
            if clipped.is_empty:
                print(f"  NOTE: {designator}: SUBTR zone lies entirely "
                      f"outside outer boundary, skipping", file=sys.stderr)
                continue
            if isinstance(clipped, ShapelyMultiPolygon):
                polys_ = list(clipped.geoms)
            elif isinstance(clipped, ShapelyPolygon):
                polys_ = [clipped]
            elif hasattr(clipped, "geoms"):
                polys_ = [g for g in clipped.geoms
                          if isinstance(g, ShapelyPolygon) and not g.is_empty]
                if not polys_:
                    print(f"  NOTE: {designator}: SUBTR clip produced no "
                          f"polygonal bits, skipping", file=sys.stderr)
                    continue
            else:
                print(f"  WARN: {designator}: SUBTR intersection yielded "
                      f"{type(clipped).__name__}, skipping", file=sys.stderr)
                continue
            unioned = (unary_union(polys_) if len(polys_) > 1 else polys_[0])
            # Snap the clipped SUBTR to the same grid BASE polys use so
            # subsequent face-overlay intersections land exactly on
            # BASE's edge rather than drifting off by a few nm of
            # floating-point noise.
            unioned = set_precision(unioned, SNAP)
            # Drop SUBTRs whose clipped footprint is a near-zero-area
            # sliver or self-retracing path — these come from source
            # polygons that graze BASE's boundary and become degenerate
            # after intersection, not airspace we want to render.
            if unioned.is_empty or unioned.area < 1e-6:
                print(f"  NOTE: {designator}: SUBTR clipped footprint "
                      f"degenerate (area={unioned.area:.2e}), skipping",
                      file=sys.stderr)
                SUA_FILTER_STATS["SUBTR clipped degenerate (area<1e-6)"][designator] += 1
                continue
            # All partial-cover SUBTRs already match BASE's ref on
            # both sides, so integer comparison is sound here.
            partial_subs_clipped.append({
                "sub_upper": sub_upper, "sub_lower": sub_lower,
                "lo_ft": parse_altitude(sub_lower)[0],
                "up_ft": parse_altitude(sub_upper)[0],
                "poly": unioned,
            })
        if partial_subs_clipped:
            partial_cover_union = unary_union(
                [s["poly"] for s in partial_subs_clipped])

    # Combined carve-out geometry subtracted from BASE band. Anything a
    # SUBTR covers (fully or partially) is removed from BASE so the
    # BASE stratum only spans the unrestricted remainder.
    base_carve = None
    if full_cover_hole_geom is not None and partial_cover_union is not None:
        base_carve = unary_union([full_cover_hole_geom, partial_cover_union])
    elif full_cover_hole_geom is not None:
        base_carve = full_cover_hole_geom
    elif partial_cover_union is not None:
        base_carve = partial_cover_union

    # strata: list of {"upper_limit", "lower_limit", "parts": [(ring, is_hole), ...]}
    # Each stratum is a separately-selectable altitude band within the SUA.
    strata = []
    result_circle_info = None

    for band_key in band_order:
        entries = bands[band_key]
        polys, raw_polys = _union_band(entries)
        if not polys:
            if band_key == base_band:
                print(f"  WARN: {designator}: BASE band union yielded no "
                      f"polygons, skipping", file=sys.stderr)
            continue

        # Apply the combined carve-out on the BASE band so BASE's
        # footprint is disjoint (in 3D) from every SUBTR's active
        # slices — shapely produces interior rings for fully-enclosed
        # SUBTRs and trims the exterior for partial-edge SUBTRs.
        if band_key == base_band and base_carve is not None:
            diffed = []
            for p in polys:
                d = p.difference(base_carve)
                if d.is_empty:
                    continue
                if isinstance(d, ShapelyPolygon):
                    diffed.append(d)
                elif isinstance(d, ShapelyMultiPolygon):
                    diffed.extend(g for g in d.geoms if not g.is_empty)
                elif hasattr(d, "geoms"):
                    for g in d.geoms:
                        if isinstance(g, ShapelyPolygon) and not g.is_empty:
                            diffed.append(g)
            polys = diffed
            if not polys:
                print(f"  WARN: {designator}: BASE band fully consumed "
                      f"by SUBTR carve-outs, skipping", file=sys.stderr)
                continue

        # Sliver filter, per-band. Absolute-area threshold: drop outer
        # polygon pieces smaller than ~0.003 NM². Real disconnected
        # airspace regions (e.g. a protrusion severed by a SUBTR)
        # remain well above this; floating-point fragments from
        # `unary_union` on overlapping sampled circles fall below.
        outer_parts = []
        for poly in polys:
            if poly.is_empty:
                continue
            if len(polys) > 1 and poly.area < OUTER_SLIVER_AREA_MIN:
                SUA_FILTER_STATS["OUTER_SLIVER_AREA_MIN (outer piece dropped)"][designator] += 1
                continue
            outer_parts.append((list(poly.exterior.coords), False))
            for interior in poly.interiors:
                outer_parts.append((list(interior.coords), True))

        # Use the first entry's altitude strings as representative — all
        # entries in this band share normalized altitude. (String form
        # may differ slightly, e.g. "GND FT SFC" vs "0 FT SFC"; the first
        # entry's strings are stable and what was in the source.)
        up_str = entries[0][3]
        lo_str = entries[0][4]
        strata.append({
            "upper_limit": up_str,
            "lower_limit": lo_str,
            "parts": list(outer_parts),
        })

        if band_key == base_band:
            # Preserve circle_info only if the whole SUA is exactly one
            # unchanged circle in the BASE band.
            if (len(additive) == 1 and additive[0][2] is not None
                    and len(outer_parts) == 1 and not outer_parts[0][1]):
                result_circle_info = additive[0][2]

    if not strata:
        return None

    # Face-overlay active-slice emission: partition the union of
    # partial-cover SUBTR footprints into faces where each face is
    # covered by a specific subset of SUBTRs. Within each face the
    # carved altitude range is the union of every member SUBTR's
    # range; active slices are [baseLo, baseUp] minus that union. All
    # faces whose active-band list contains a given band are merged
    # into one stratum so a band appears as a single continuous
    # polygon regardless of which SUBTRs produced it.
    if partial_subs_clipped and base_lo_val is not None and base_up_val is not None:
        def _merge_ranges(rs):
            rs = sorted((s, e) for s, e in rs if s is not None and e is not None and s < e)
            out = []
            for s, e in rs:
                if out and s <= out[-1][1]:
                    out[-1] = (out[-1][0], max(out[-1][1], e))
                else:
                    out.append((s, e))
            return out

        def _complement(rs, lo, hi):
            merged = _merge_ranges(rs)
            result = []
            cur = lo
            for s, e in merged:
                if e <= cur:
                    continue
                if s > cur:
                    result.append((cur, min(s, hi)))
                cur = max(cur, e)
                if cur >= hi:
                    break
            if cur < hi:
                result.append((cur, hi))
            return result

        # Area threshold below which a polygon is treated as a
        # degenerate artifact (a thin sliver from a near-miss clip, or
        # a self-retracing near-zero-area loop that shapely still
        # reports as valid). 1e-6 deg² ≈ 10,000 m² ≈ 0.003 NM²; real
        # SUA features are always orders of magnitude larger.
        FACE_AREA_MIN = 1e-6

        def _collect_polys(geom):
            if geom.is_empty:
                return []
            if isinstance(geom, ShapelyPolygon):
                if geom.area >= FACE_AREA_MIN:
                    return [geom]
                SUA_FILTER_STATS["FACE_AREA_MIN (overlay sliver)"][designator] += 1
                return []
            if isinstance(geom, ShapelyMultiPolygon):
                out = []
                for g in geom.geoms:
                    if g.is_empty:
                        continue
                    if g.area >= FACE_AREA_MIN:
                        out.append(g)
                    else:
                        SUA_FILTER_STATS["FACE_AREA_MIN (overlay sliver)"][designator] += 1
                return out
            if hasattr(geom, "geoms"):
                out = []
                for g in geom.geoms:
                    if not isinstance(g, ShapelyPolygon) or g.is_empty:
                        continue
                    if g.area >= FACE_AREA_MIN:
                        out.append(g)
                    else:
                        SUA_FILTER_STATS["FACE_AREA_MIN (overlay sliver)"][designator] += 1
                return out
            return []

        # Synthesize the altitude string for one bound of an active
        # slice. The slice's bounds come from BASE.lower/BASE.upper or
        # from SUBTR.lower/SUBTR.upper (partial-cover SUBTRs share
        # BASE's refs, so SUBTR.upper has base_up_ref and SUBTR.lower
        # has base_lo_ref). `is_lower` selects which side we're
        # synthesizing — for the slice's lower bound, the value is
        # either base_lo_val (use base_lo_ref) or a SUBTR.upper
        # (use base_up_ref). For the upper bound, the value is either
        # base_up_val (use base_up_ref) or a SUBTR.lower (use base_lo_ref).
        def _alt_to_str(ft, is_lower):
            if ft is None:
                return ""
            if is_lower:
                if ft == base_lo_val:
                    return lower_limit
                ref = base_up_ref  # came from a SUBTR.upper
            else:
                if ft == base_up_val:
                    return upper_limit
                ref = base_lo_ref  # came from a SUBTR.lower
            if ft == 0 and ref == "SFC":
                return "GND FT SFC"
            if ft >= ALT_UNLIMITED_FT:
                return "UNL OTHER"
            return f"{ft} FT {ref}"

        n = len(partial_subs_clipped)
        # Collect (lo, hi) active band -> list of face polygons across
        # the overlay. Brute-force 2^n subset enumeration; n is small
        # in practice (<=10 across the current corpus).
        band_to_polys = {}  # (lo_ft, hi_ft) -> [ShapelyPolygon ...]
        for mask in range(1, 1 << n):
            members = [i for i in range(n) if mask & (1 << i)]
            face = partial_subs_clipped[members[0]]["poly"]
            skip = False
            for i in members[1:]:
                face = face.intersection(partial_subs_clipped[i]["poly"])
                if face.is_empty:
                    skip = True
                    break
            if skip or face.is_empty:
                continue
            for j in range(n):
                if mask & (1 << j):
                    continue
                face = face.difference(partial_subs_clipped[j]["poly"])
                if face.is_empty:
                    break
            face_polys = _collect_polys(face)
            if not face_polys:
                continue
            carved = [(partial_subs_clipped[i]["lo_ft"],
                       partial_subs_clipped[i]["up_ft"]) for i in members]
            for band in _complement(carved, base_lo_val, base_up_val):
                band_to_polys.setdefault(band, []).extend(face_polys)

        for (lo_ft, hi_ft), polys_ in band_to_polys.items():
            if not polys_:
                continue
            merged = unary_union(polys_) if len(polys_) > 1 else polys_[0]
            stratum_parts = []
            for p in _collect_polys(merged):
                stratum_parts.append((list(p.exterior.coords), False))
                for interior in p.interiors:
                    stratum_parts.append((list(interior.coords), True))
            if not stratum_parts:
                continue
            lo_str = _alt_to_str(lo_ft, is_lower=True)
            up_str = _alt_to_str(hi_ft, is_lower=False)
            strata.append({
                "upper_limit": up_str,
                "lower_limit": lo_str,
                "parts": stratum_parts,
            })

    # Standalone SUBTRs (altitude reference does not match BASE's, so
    # we cannot compare values without terrain). Each is emitted as a
    # single stratum with its raw altitude band, footprint clipped to
    # the BASE outline. No subtraction from BASE, no overlay math —
    # the user sees both the BASE region and this overlapping stratum.
    if standalone_subs and outer_union is not None and not outer_union.is_empty:
        standalone_subs.sort(key=lambda c: c[0])
        for _, shape, sub_upper, sub_lower in standalone_subs:
            if len(shape) < 3:
                continue
            sub_poly = make_valid(ShapelyPolygon(shape))
            if sub_poly.is_empty:
                continue
            clipped = sub_poly.intersection(outer_union)
            if clipped.is_empty:
                print(f"  NOTE: {designator}: standalone SUBTR (mixed-ref) "
                      f"lies outside BASE, skipping", file=sys.stderr)
                continue
            if isinstance(clipped, ShapelyMultiPolygon):
                polys_ = [g for g in clipped.geoms if not g.is_empty]
            elif isinstance(clipped, ShapelyPolygon):
                polys_ = [clipped]
            elif hasattr(clipped, "geoms"):
                polys_ = [g for g in clipped.geoms
                          if isinstance(g, ShapelyPolygon) and not g.is_empty]
            else:
                continue
            stratum_parts = []
            for p in polys_:
                if p.is_empty or p.area < 1e-6:
                    continue
                stratum_parts.append((list(p.exterior.coords), False))
                for interior in p.interiors:
                    stratum_parts.append((list(interior.coords), True))
            if stratum_parts:
                strata.append({
                    "upper_limit": sub_upper,
                    "lower_limit": sub_lower,
                    "parts": stratum_parts,
                })
                SUA_FILTER_STATS["standalone SUBTR (mixed-ref)"][designator] += 1

    return {
        "designator": designator,
        "name": name,
        "sua_type": sua_type,
        "upper_limit": upper_limit,
        "lower_limit": lower_limit,
        "min_alt_limit": min_alt_limit,
        "max_alt_limit": max_alt_limit,
        "admin_area": admin_area,
        "city": saa_city,
        "military": military,
        "activity": activity,
        "status": status,
        "working_hours": working_hours,
        "compliant_icao": compliant_icao,
        "legal_note": legal_note,
        "strata": strata,
        "circle_info": result_circle_info,
    }


XLINK = "http://www.w3.org/1999/xlink"


def _parse_radio_channels(root, gml_id_map):
    """Parse RadioCommunicationChannel elements.

    Returns a dict mapping gml:id -> {mode, tx_freq, rx_freq}.
    """
    channels = {}
    for rcc in root.iter(f"{{{AIXM}}}RadioCommunicationChannel"):
        gid = rcc.get(f"{{{GML}}}id", "")
        ts = rcc.find(f".//{{{AIXM}}}RadioCommunicationChannelTimeSlice")
        if ts is None:
            continue
        mode = _text(ts, f"{{{AIXM}}}mode")
        tx = _text(ts, f"{{{AIXM}}}frequencyTransmission")
        rx = _text(ts, f"{{{AIXM}}}frequencyReception")
        # AIXM declares MHz in `uom` on these elements; the FAA
        # corpus is consistently MHz, so we keep the source text
        # as-is and don't read the uom.
        channels[gid] = {
            "mode": mode,
            "tx_freq": tx,
            "rx_freq": rx,
        }
    return channels


def _parse_units(root):
    """Parse Unit elements.

    Returns a dict mapping gml:id -> {name, type, military}.
    """
    units = {}
    for unit in root.iter(f"{{{AIXM}}}Unit"):
        gid = unit.get(f"{{{GML}}}id", "")
        ts = unit.find(f".//{{{AIXM}}}UnitTimeSlice")
        if ts is None:
            continue
        units[gid] = {
            "name": _text(ts, f"{{{AIXM}}}name"),
            "type": _text(ts, f"{{{AIXM}}}type"),
            "military": _text(ts, f"{{{AIXM}}}military"),
        }
    return units


def _resolve_href(href, lookup):
    """Resolve an xlink:href like '#Unit1' against a gml:id lookup dict."""
    if not href:
        return None
    key = href.lstrip("#")
    return lookup.get(key)


def _parse_services(root, units, channels):
    """Parse AirTrafficControlService and InformationService elements.

    Returns a list of service dicts with unit and channel info resolved.
    """
    services = []

    for svc in root.iter(f"{{{AIXM}}}AirTrafficControlService"):
        ts = svc.find(f".//{{{AIXM}}}AirTrafficControlServiceTimeSlice")
        if ts is None:
            continue
        stype = _text(ts, f"{{{AIXM}}}type")
        sp = ts.find(f"{{{AIXM}}}serviceProvider")
        sp_href = sp.get(f"{{{XLINK}}}href", "") if sp is not None else ""
        unit = _resolve_href(sp_href, units)
        services.append({
            "service_type": stype or "ACS",
            "unit_name": unit["name"] if unit else "",
            "unit_type": unit["type"] if unit else "",
        })

    for svc in root.iter(f"{{{AIXM}}}InformationService"):
        ts = svc.find(f".//{{{AIXM}}}InformationServiceTimeSlice")
        if ts is None:
            continue
        stype = _text(ts, f"{{{AIXM}}}type")
        sp = ts.find(f"{{{AIXM}}}serviceProvider")
        sp_href = sp.get(f"{{{XLINK}}}href", "") if sp is not None else ""
        unit = _resolve_href(sp_href, units)
        services.append({
            "service_type": stype or "INFO",
            "unit_name": unit["name"] if unit else "",
            "unit_type": unit["type"] if unit else "",
        })

    return services


def _parse_timesheets(root):
    """Parse Timesheet entries from AirspaceUsage elements.

    Returns a list of schedule dicts with day, start/end times, timezone,
    and DST flag.
    """
    schedules = []

    for usage_ts in root.iter(f"{{{AIXM}}}AirspaceUsageTimeSlice"):
        # DST flag from the AirspaceUsage extension
        dst_flag = ""
        for ext in usage_ts.findall(f"{{{AIXM}}}extension"):
            dst = ext.find(f".//{{{SAA}}}daylightSavings")
            if dst is not None and dst.text:
                dst_flag = dst.text.strip()

        for timesheet in usage_ts.iter(f"{{{AIXM}}}Timesheet"):
            day = _text(timesheet, f"{{{AIXM}}}day")
            day_til = _text(timesheet, f"{{{AIXM}}}dayTil")
            start_time = _text(timesheet, f"{{{AIXM}}}startTime")
            end_time = _text(timesheet, f"{{{AIXM}}}endTime")
            # Event-relative activations: a Timesheet may name a
            # solar event (SR=sunrise, SS=sunset) instead of (or in
            # addition to) a clock time, with an optional offset in
            # minutes ("60 min before sunrise", etc.). The render
            # path joins event + offset into "SR-60" / "SS+60" etc.
            start_event = _text(timesheet, f"{{{AIXM}}}startEvent")
            end_event = _text(timesheet, f"{{{AIXM}}}endEvent")
            start_event_offset = _text(
                timesheet, f"{{{AIXM}}}startTimeRelativeEvent")
            end_event_offset = _text(
                timesheet, f"{{{AIXM}}}endTimeRelativeEvent")
            time_ref = _text(timesheet, f"{{{AIXM}}}timeReference")

            # UTC offset from TimesheetExtension
            time_offset = ""
            for ext in timesheet.findall(f"{{{AIXM}}}extension"):
                offset_el = ext.find(f".//{{{SAA}}}timeOffset")
                if offset_el is not None and offset_el.text:
                    time_offset = offset_el.text.strip()

            schedules.append({
                "day": day,
                "day_til": day_til,
                "start_time": start_time,
                "end_time": end_time,
                "start_event": start_event,
                "end_event": end_event,
                "start_event_offset": start_event_offset,
                "end_event_offset": end_event_offset,
                "time_ref": time_ref,
                "time_offset": time_offset,
                "dst_flag": dst_flag,
            })

    return schedules


def _parse_freq_allocations(root, channels):
    """Resolve per-airspace channel allocations.

    Returns `(by_airspace, fallback)` where:
      `by_airspace` — dict {airspace_gml_id → [freq_dict, ...]} from
                      `RadioCommunicationChannelAllocation` blocks.
                      Each freq_dict carries `comm_allowed` and
                      `charted` flags from the allocation.
      `fallback`    — list of freq_dicts for channels that no
                      allocation references; legacy "file-scope"
                      channels that fall back to all SUAs.
    """
    # Per-airspace mapping: airspace gml:id → list of freq dicts.
    # `RadioCommunicationChannelAllocation` blocks pin each channel
    # to a specific airspace via `<associatedAirspace xlink:href>`,
    # along with `communicationAllowed` (CIVIL/MIL) and `charted`
    # (YES/NO) flags. In multi-airspace files (e.g. R-4812) this is
    # the only correct way to determine which channels belong to
    # which airspace.
    by_airspace = {}      # gml_id → [freq_dict, ...]
    seen_per_airspace = {}  # gml_id → set of channel hrefs seen
    fallback = []         # channels with no allocation block at all

    for alloc in root.iter(
            f"{{{SAA}}}RadioCommunicationChannelAllocation"):
        airspace_ref = alloc.find(f"{{{SAA}}}associatedAirspace")
        if airspace_ref is None:
            continue
        airspace_href = airspace_ref.get(f"{{{XLINK}}}href", "")
        airspace_gid = airspace_href.lstrip("#")
        if not airspace_gid:
            continue
        details = alloc.find(f"{{{SAA}}}allocatedChannelDetails")
        if details is None:
            continue
        for sch in details.iter(f"{{{SAA}}}SaaRadioCommunicationChannel"):
            comm = _text(sch, f"{{{SAA}}}communicationAllowed")
            charted = _text(sch, f"{{{SAA}}}charted")
            ch_ref = sch.find(f"{{{SAA}}}associatedChannel")
            if ch_ref is None:
                continue
            ch_href = ch_ref.get(f"{{{XLINK}}}href", "")
            ch = _resolve_href(ch_href, channels)
            if ch is None:
                continue
            seen = seen_per_airspace.setdefault(airspace_gid, set())
            if ch_href in seen:
                continue
            seen.add(ch_href)
            entry = ch.copy()
            entry["comm_allowed"] = comm
            entry["charted"] = charted
            by_airspace.setdefault(airspace_gid, []).append(entry)

    # Channels not bound by any allocation: keep around for SUAs
    # that have no per-airspace allocation but still reference
    # channels via InformationService.radioCommunication. These
    # are file-scope rather than airspace-scope.
    referenced = set()
    for href in (e.get(f"{{{XLINK}}}href", "")
                 for alloc in root.iter(
                     f"{{{SAA}}}RadioCommunicationChannelAllocation")
                 for e in alloc.iter(f"{{{SAA}}}associatedChannel")):
        referenced.add(href.lstrip("#"))
    for gid, ch in channels.items():
        if gid not in referenced:
            entry = ch.copy()
            entry["comm_allowed"] = ""
            entry["charted"] = ""
            fallback.append(entry)

    return by_airspace, fallback


def parse_aixm_sua(xml_data):
    """Parse AIXM 5.0 SUA XML data.

    xml_data is bytes or a file-like object. A single file may contain
    multiple Airspace elements.
    Returns a list of parsed airspace dicts (may be empty).

    The AIXM document has sibling hasMember elements at the root level:
    OrganisationAuthority, Airspace, Unit (controlling/ARTCC), AirspaceUsage,
    RadioCommunicationChannel, AirTrafficControlService, InformationService.
    """
    root = ET.fromstring(xml_data)

    # Extract controlling authority from OrganisationAuthority elements
    controlling_auth = ""
    for oa_ts in root.iter(f"{{{AIXM}}}OrganisationAuthorityTimeSlice"):
        oa_name = _text(oa_ts, f"{{{AIXM}}}name")
        oa_type = _text(oa_ts, f"{{{AIXM}}}type")
        if oa_type == "NTL_AUTH" and oa_name:
            controlling_auth = oa_name
            break

    # Extract metadata from Unit elements (military status, ICAO compliance)
    military = ""
    compliant_icao = ""
    units = _parse_units(root)
    for unit in units.values():
        if unit["type"] == "MILOPS" and not military:
            military = unit["military"]
        elif unit["type"] == "ARTCC" and not compliant_icao:
            compliant_icao = _text(root, f"{{{AIXM}}}compliantICAO")

    # Extract metadata from AirspaceUsage (activity, status, working hours)
    activity = ""
    status = ""
    working_hours = ""
    for usage_ts in root.iter(f"{{{AIXM}}}AirspaceUsageTimeSlice"):
        if not activity:
            activity = _text(usage_ts, f"{{{AIXM}}}activity")
        if not status:
            status = _text(usage_ts, f"{{{AIXM}}}statusActivation")
        if not working_hours:
            working_hours = _text(usage_ts, f"{{{AIXM}}}workingHours")

    # Parse radio channels, services, and timesheets
    channels = _parse_radio_channels(root, {})
    services = _parse_services(root, units, channels)
    timesheets = _parse_timesheets(root)
    freqs_by_airspace, freqs_fallback = _parse_freq_allocations(root, channels)

    # A file's canonical Airspace is the one not referenced by any
    # <theAirspace xlink:href="#..."> within the file. Other <Airspace>
    # elements are neighbors inlined by reference (e.g. R-4812's file
    # inlines R-4804A and R-4810) and have their own canonical files.
    referenced_ids = set()
    for ref in root.iter(f"{{{AIXM}}}theAirspace"):
        href = ref.get(f"{{{XLINK}}}href", "")
        if href.startswith("#"):
            referenced_ids.add(href[1:])

    # Index all airspaces in the file by gml:id so cross-airspace SUBTR
    # references (contributorAirspace xlink) can be resolved.
    airspace_lookup = {
        a.get(f"{{{GML}}}id"): a
        for a in root.iter(f"{{{AIXM}}}Airspace")
        if a.get(f"{{{GML}}}id")
    }

    # Source-data assumption checks. Every entry asserts a value the
    # rest of the pipeline relies on; if a future data drop ever
    # introduces a different value, we want a loud warning instead
    # of silently mis-interpreting the source.
    for elem in root.iter(f"{{{AIXM}}}altitudeInterpretation"):
        # `BETWEEN` means lower ≤ altitude ≤ upper. Anything else
        # would require new combination logic.
        v = (elem.text or "").strip()
        if v and v != "BETWEEN":
            print(f"  WARN: unsupported altitudeInterpretation={v!r} "
                  f"— pipeline assumes BETWEEN", file=sys.stderr)
    for elem in root.iter(f"{{{AIXM}}}dependency"):
        # Inside `AirspaceVolumeDependency`. Only `HORZ_PROJECTION`
        # is observed; that's what `_collect_additive_shapes` handles
        # via the xlink-resolution path (R-4812).
        v = (elem.text or "").strip()
        if v and v != "HORZ_PROJECTION":
            print(f"  WARN: unsupported AirspaceVolumeDependency dependency="
                  f"{v!r} — pipeline only handles HORZ_PROJECTION",
                  file=sys.stderr)
    for elem in root.iter(f"{{{AIXM}}}Surface"):
        # Coordinates are stored as raw lon/lat with no projection
        # step. Anything other than CRS84 (WGS84 lon/lat) would put
        # geometry in the wrong place.
        v = (elem.get("srsName") or "").strip()
        if v and v.upper() != "URN:OGC:DEF:CRS:OGC:1.3:CRS84":
            print(f"  WARN: unsupported Surface srsName={v!r} "
                  f"— pipeline assumes WGS84 lon/lat (CRS84)",
                  file=sys.stderr)
    for tag in ("ArcByCenterPoint", "CircleByCenterPoint"):
        # `parse_ring_points` samples a single arc per element; the
        # multi-arc case would need a different traversal.
        for elem in root.iter(f"{{{GML}}}{tag}"):
            v = (elem.get("numArc") or "").strip()
            if v and v != "1":
                print(f"  WARN: gml:{tag} numArc={v!r} — pipeline "
                      f"assumes a single arc per element", file=sys.stderr)

    results = []
    for airspace in root.iter(f"{{{AIXM}}}Airspace"):
        if airspace.get(f"{{{GML}}}id") in referenced_ids:
            continue
        result = _parse_one_airspace(airspace, airspace_lookup)
        if result is not None:
            result["controlling_authority"] = controlling_auth
            result["military"] = result["military"] or military
            result["compliant_icao"] = result["compliant_icao"] or compliant_icao
            result["activity"] = result["activity"] or activity
            result["status"] = result["status"] or status
            result["working_hours"] = result["working_hours"] or working_hours
            # Per-airspace channel allocation: pull this airspace's
            # bound channels first; if none exist, fall back to any
            # file-scope channels (channels declared but never bound).
            ag = airspace.get(f"{{{GML}}}id", "")
            airspace_freqs = list(freqs_by_airspace.get(ag, []))
            if not airspace_freqs:
                airspace_freqs = list(freqs_fallback)
            result["freqs"] = airspace_freqs
            result["services"] = services
            result["schedules"] = timesheets
            results.append(result)
    return results


def build_sua(conn, aixm_zf):
    """Import SUA polygon boundaries from AIXM 5.0 XML files.

    aixm_zf is the outer AIXM ZIP. The XML files are nested:
    aixm5.0.zip / SaaSubscriberFile.zip / Saa_Sub_File.zip / *.xml
    """
    # Navigate nested ZIPs to reach the XML files
    mid_name = next(n for n in aixm_zf.namelist() if n.endswith('.zip'))
    mid_zf = zipfile.ZipFile(io.BytesIO(aixm_zf.read(mid_name)))
    inner_name = next(n for n in mid_zf.namelist() if n.endswith('.zip'))
    inner_zf = zipfile.ZipFile(io.BytesIO(mid_zf.read(inner_name)))

    xml_files = sorted(n for n in inner_zf.namelist() if n.endswith('.xml'))
    if not xml_files:
        print("  No XML files found in AIXM archive")
        return

    conn.execute("DROP TABLE IF EXISTS SUA_BASE")
    # MIN_ALT_LIMIT / MAX_ALT_LIMIT carry the AIXM minimumLimit /
    # maximumLimit pair from the BASE volume — terrain-clearance
    # constraints (typically "min N ft AGL") supplementing the MSL
    # band. Empty for SUAs that don't declare them (most).
    conn.execute("""
        CREATE TABLE SUA_BASE (
            SUA_ID INTEGER PRIMARY KEY,
            DESIGNATOR TEXT,
            NAME TEXT,
            SUA_TYPE TEXT,
            UPPER_LIMIT TEXT,
            LOWER_LIMIT TEXT,
            MIN_ALT_LIMIT TEXT,
            MAX_ALT_LIMIT TEXT,
            CONTROLLING_AUTHORITY TEXT,
            ADMIN_AREA TEXT,
            CITY TEXT,
            MILITARY TEXT,
            ACTIVITY TEXT,
            STATUS TEXT,
            WORKING_HOURS TEXT,
            ICAO_COMPLIANT TEXT,
            LEGAL_NOTE TEXT
        )
    """)

    conn.execute("DROP TABLE IF EXISTS SUA_STRATUM")
    # `*_FT_VAL` is feet (or hundreds-of-feet for STD/FL); `*_FT_REF`
    # is one of MSL/SFC/STD/OTHER. Two altitudes may be compared
    # numerically only when their refs match.
    conn.execute("""
        CREATE TABLE SUA_STRATUM (
            STRATUM_ID INTEGER PRIMARY KEY,
            SUA_ID INTEGER,
            STRATUM_ORDER INTEGER,
            UPPER_LIMIT TEXT,
            LOWER_LIMIT TEXT,
            UPPER_FT_VAL INTEGER,
            UPPER_FT_REF TEXT,
            LOWER_FT_VAL INTEGER,
            LOWER_FT_REF TEXT
        )
    """)

    conn.execute("DROP TABLE IF EXISTS SUA_SHP")
    conn.execute("""
        CREATE TABLE SUA_SHP (
            SUA_ID INTEGER,
            STRATUM_ID INTEGER,
            PART_NUM INTEGER,
            IS_HOLE INTEGER,
            POINT_SEQ INTEGER,
            LON_DECIMAL REAL,
            LAT_DECIMAL REAL
        )
    """)

    conn.execute("DROP TABLE IF EXISTS SUA_CIRCLE")
    conn.execute("""
        CREATE TABLE SUA_CIRCLE (
            SUA_ID INTEGER,
            STRATUM_ID INTEGER,
            PART_NUM INTEGER,
            CENTER_LON REAL,
            CENTER_LAT REAL,
            RADIUS_NM REAL
        )
    """)

    conn.execute("DROP TABLE IF EXISTS SUA_FREQ")
    # Frequencies stored as raw source text (matches the convention
    # used by NAV_BASE.FREQ, ILS_BASE.LOC_FREQ, FRQ.FREQ, etc.).
    # We never compare or do arithmetic on them in this app — only
    # display — so a string round-trips without any precision loss.
    # COMM_ALLOWED: "CIVIL"/"MIL"/"" (from the per-airspace
    # RadioCommunicationChannelAllocation block).
    # CHARTED: "YES"/"NO"/"" — whether the channel is published on
    # the chart for this airspace.
    conn.execute("""
        CREATE TABLE SUA_FREQ (
            SUA_ID INTEGER,
            FREQ_SEQ INTEGER,
            MODE TEXT,
            TX_FREQ TEXT,
            RX_FREQ TEXT,
            COMM_ALLOWED TEXT,
            CHARTED TEXT,
            PRIMARY KEY (SUA_ID, FREQ_SEQ)
        )
    """)

    conn.execute("DROP TABLE IF EXISTS SUA_SCHEDULE")
    # DAY_TIL: end day of an inclusive day-of-week range (e.g. day=MON,
    # day_til=TUE means "Mon through Tue"). Empty when a single day.
    # START_EVENT/END_EVENT: solar event token (SR/SS) when the
    # activation is anchored to sunrise/sunset rather than (or in
    # addition to) a clock time. *_EVENT_OFFSET: minutes offset
    # relative to that event (e.g. -60 = "60 min before sunrise").
    conn.execute("""
        CREATE TABLE SUA_SCHEDULE (
            SUA_ID INTEGER,
            SCHED_SEQ INTEGER,
            DAY_OF_WEEK TEXT,
            DAY_TIL TEXT,
            START_TIME TEXT,
            END_TIME TEXT,
            START_EVENT TEXT,
            END_EVENT TEXT,
            START_EVENT_OFFSET TEXT,
            END_EVENT_OFFSET TEXT,
            TIME_REF TEXT,
            TIME_OFFSET TEXT,
            DST_FLAG TEXT,
            PRIMARY KEY (SUA_ID, SCHED_SEQ)
        )
    """)

    conn.execute("DROP TABLE IF EXISTS SUA_SERVICE")
    conn.execute("""
        CREATE TABLE SUA_SERVICE (
            SUA_ID INTEGER,
            SVC_SEQ INTEGER,
            SERVICE_TYPE TEXT,
            UNIT_NAME TEXT,
            UNIT_TYPE TEXT,
            PRIMARY KEY (SUA_ID, SVC_SEQ)
        )
    """)

    epsilon = 0.0001
    total_before = 0
    total_after = 0
    base_rows = []
    stratum_rows = []
    shp_rows = []
    circle_rows = []
    freq_rows = []
    schedule_rows = []
    service_rows = []
    skipped = 0
    next_stratum_id = 1

    for xml_name in xml_files:
        results = parse_aixm_sua(inner_zf.read(xml_name))
        if not results:
            skipped += 1
            continue

        for result in results:
            sua_id = len(base_rows) + 1
            base_rows.append((
                sua_id,
                result["designator"],
                result["name"],
                result["sua_type"],
                result["upper_limit"],
                result["lower_limit"],
                result["min_alt_limit"],
                result["max_alt_limit"],
                result["controlling_authority"],
                result["admin_area"],
                result["city"],
                result["military"],
                result["activity"],
                result["status"],
                result["working_hours"],
                result["compliant_icao"],
                result["legal_note"],
            ))

            # A pure-circle SUA: one BASE stratum, one circle part.
            ci = result.get("circle_info")
            circle_stratum_id = None
            if ci is not None:
                r = ci["radius"]
                uom = ci["uom"]
                if uom == "KM":
                    r /= 1.852
                elif uom == "M":
                    r /= 1852.0
                elif uom == "FT":
                    r *= 0.3048 / 1852.0
                elif uom == "MI":
                    r *= 1609.344 / 1852.0
                elif uom != "NM":
                    r /= 1.852  # assume KM if unknown
                # Allocate the BASE stratum upfront so the circle part
                # can reference it. Matching band will find the same id.
                circle_stratum_id = next_stratum_id
                next_stratum_id += 1
                _ulim_v, _ulim_r = parse_altitude(result["upper_limit"])
                _llim_v, _llim_r = parse_altitude(result["lower_limit"])
                stratum_rows.append((
                    circle_stratum_id, sua_id, 0,
                    result["upper_limit"], result["lower_limit"],
                    _ulim_v, _ulim_r, _llim_v, _llim_r,
                ))
                circle_rows.append((sua_id, circle_stratum_id, 0,
                                    ci["lon"], ci["lat"], r))

            for stratum_order, stratum in enumerate(result["strata"]):
                if stratum_order == 0 and circle_stratum_id is not None:
                    # BASE stratum already registered as the circle's parent.
                    stratum_id = circle_stratum_id
                else:
                    stratum_id = next_stratum_id
                    next_stratum_id += 1
                    _ulim_v, _ulim_r = parse_altitude(stratum["upper_limit"])
                    _llim_v, _llim_r = parse_altitude(stratum["lower_limit"])
                    stratum_rows.append((
                        stratum_id, sua_id, stratum_order,
                        stratum["upper_limit"], stratum["lower_limit"],
                        _ulim_v, _ulim_r, _llim_v, _llim_r,
                    ))
                part_num = 0
                for ring, is_hole in stratum["parts"]:
                    for copy in handle_antimeridian(ring):
                        total_before += len(copy)
                        _n_pre_simp = len(copy)
                        simplified = simplify_ring(copy, epsilon)
                        if len(simplified) != _n_pre_simp:
                            SUA_FILTER_STATS["simplify_ring (Douglas-Peucker)"][result["designator"]] += (_n_pre_simp - len(simplified))
                        # Drop degenerate parts (fewer than 3 distinct
                        # vertices) — typically a SUBTR zone that
                        # simplified down to a filament.
                        unique_pts = len({(lon, lat) for lon, lat in simplified})
                        if unique_pts < 3:
                            SUA_FILTER_STATS["unique_pts<3 (degenerate part dropped)"][result["designator"]] += 1
                            continue
                        total_after += len(simplified)
                        for point_seq, (lon, lat) in enumerate(simplified):
                            shp_rows.append((sua_id, stratum_id, part_num,
                                             1 if is_hole else 0,
                                             point_seq, lon, lat))
                        part_num += 1

            # Frequencies
            for seq, freq in enumerate(result.get("freqs", []), 1):
                freq_rows.append((
                    sua_id, seq,
                    freq["mode"],
                    freq["tx_freq"],
                    freq["rx_freq"],
                    freq.get("comm_allowed", ""),
                    freq.get("charted", ""),
                ))

            # Schedules
            for seq, sched in enumerate(result.get("schedules", []), 1):
                schedule_rows.append((
                    sua_id, seq,
                    sched["day"],
                    sched["day_til"],
                    sched["start_time"],
                    sched["end_time"],
                    sched["start_event"],
                    sched["end_event"],
                    sched["start_event_offset"],
                    sched["end_event_offset"],
                    sched["time_ref"],
                    sched["time_offset"],
                    sched["dst_flag"],
                ))

            # Services
            for seq, svc in enumerate(result.get("services", []), 1):
                service_rows.append((
                    sua_id, seq,
                    svc["service_type"],
                    svc["unit_name"],
                    svc["unit_type"],
                ))

    conn.executemany(
        "INSERT INTO SUA_BASE VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
        base_rows,
    )
    conn.executemany(
        "INSERT INTO SUA_STRATUM VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
        stratum_rows,
    )
    conn.executemany(
        "INSERT INTO SUA_SHP VALUES (?, ?, ?, ?, ?, ?, ?)",
        shp_rows,
    )
    conn.executemany(
        "INSERT INTO SUA_CIRCLE VALUES (?, ?, ?, ?, ?, ?)",
        circle_rows,
    )
    conn.executemany(
        "INSERT INTO SUA_FREQ VALUES (?, ?, ?, ?, ?, ?, ?)",
        freq_rows,
    )
    conn.executemany(
        "INSERT INTO SUA_SCHEDULE VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
        schedule_rows,
    )
    conn.executemany(
        "INSERT INTO SUA_SERVICE VALUES (?, ?, ?, ?, ?)",
        service_rows,
    )

    print(f"  Parsed {len(xml_files)} files, {skipped} skipped")
    print(f"  SUA_BASE: {len(base_rows)} airspaces")
    print(f"  SUA_STRATUM: {len(stratum_rows)} strata")
    print(f"  SUA_SHP: {len(shp_rows)} shape points")
    print(f"  SUA_CIRCLE: {len(circle_rows)} circle airspaces")
    print(f"  SUA_FREQ: {len(freq_rows)} frequency entries")
    print(f"  SUA_SCHEDULE: {len(schedule_rows)} schedule entries")
    print(f"  SUA_SERVICE: {len(service_rows)} service entries")
    if total_before > 0:
        print(f"  Simplified: {total_before} -> {total_after} points "
              f"({100 * total_after / total_before:.1f}%)")

    # R-tree on SUA-level bounding boxes (for listing all SUAs in view).
    conn.execute("""
        CREATE VIRTUAL TABLE SUA_BASE_RTREE USING rtree(
            id, min_lon, max_lon, min_lat, max_lat
        )
    """)
    conn.execute("""
        INSERT INTO SUA_BASE_RTREE (id, min_lon, max_lon, min_lat, max_lat)
        SELECT SUA_ID,
               MIN(LON_DECIMAL), MAX(LON_DECIMAL),
               MIN(LAT_DECIMAL), MAX(LAT_DECIMAL)
        FROM SUA_SHP
        GROUP BY SUA_ID
    """)

    # R-tree on stratum-level bounding boxes (for per-stratum picking).
    conn.execute("""
        CREATE VIRTUAL TABLE SUA_STRATUM_RTREE USING rtree(
            id, min_lon, max_lon, min_lat, max_lat
        )
    """)
    conn.execute("""
        INSERT INTO SUA_STRATUM_RTREE (id, min_lon, max_lon, min_lat, max_lat)
        SELECT STRATUM_ID,
               MIN(LON_DECIMAL), MAX(LON_DECIMAL),
               MIN(LAT_DECIMAL), MAX(LAT_DECIMAL)
        FROM SUA_SHP
        GROUP BY STRATUM_ID
    """)
    # Circle strata need their bbox too — computed from center+radius.
    # 1 NM ≈ 1/60 deg latitude; lon is approx scaled by cos(lat).
    import math as _math
    for row in conn.execute(
            "SELECT s.STRATUM_ID, c.CENTER_LON, c.CENTER_LAT, c.RADIUS_NM "
            "FROM SUA_STRATUM s JOIN SUA_CIRCLE c ON c.STRATUM_ID = s.STRATUM_ID "
            "WHERE s.STRATUM_ID NOT IN (SELECT id FROM SUA_STRATUM_RTREE)"
            ).fetchall():
        sid, lon, lat, rad_nm = row
        dlat = rad_nm / 60.0
        dlon = rad_nm / (60.0 * max(0.01, _math.cos(_math.radians(lat))))
        conn.execute(
            "INSERT INTO SUA_STRATUM_RTREE (id, min_lon, max_lon, min_lat, max_lat) "
            "VALUES (?, ?, ?, ?, ?)",
            (sid, lon - dlon, lon + dlon, lat - dlat, lat + dlat))

    conn.execute("CREATE INDEX idx_sua_shp ON SUA_SHP(SUA_ID)")
    conn.execute("CREATE INDEX idx_sua_shp_stratum ON SUA_SHP(STRATUM_ID)")
    conn.execute("CREATE INDEX idx_sua_stratum_sua ON SUA_STRATUM(SUA_ID)")
    conn.execute("CREATE INDEX idx_sua_circle_stratum ON SUA_CIRCLE(STRATUM_ID)")
    conn.execute("CREATE INDEX idx_sua_freq ON SUA_FREQ(SUA_ID)")
    conn.execute("CREATE INDEX idx_sua_schedule ON SUA_SCHEDULE(SUA_ID)")
    conn.execute("CREATE INDEX idx_sua_service ON SUA_SERVICE(SUA_ID)")

    # Subdivided segments for rendering (tighter R-tree bboxes).
    # Keyed by stratum so the renderer can style per-stratum later.
    # Circles are not subdivided — they use SUA_CIRCLE directly.
    conn.execute("DROP TABLE IF EXISTS SUA_SEG")
    conn.execute("""
        CREATE TABLE SUA_SEG (
            SEG_ID INTEGER,
            STRATUM_ID INTEGER,
            SUA_ID INTEGER,
            SUA_TYPE TEXT,
            UPPER_FT_VAL INTEGER,
            UPPER_FT_REF TEXT,
            LOWER_FT_VAL INTEGER,
            LOWER_FT_REF TEXT,
            POINT_SEQ INTEGER,
            LON_DECIMAL REAL,
            LAT_DECIMAL REAL
        )
    """)

    # Build lookup from (STRATUM_ID, PART_NUM) to ring points.
    sua_rings = {}
    for row in conn.execute(
            "SELECT STRATUM_ID, PART_NUM, LON_DECIMAL, LAT_DECIMAL "
            "FROM SUA_SHP ORDER BY STRATUM_ID, PART_NUM, POINT_SEQ"):
        sua_rings.setdefault((row[0], row[1]), []).append((row[2], row[3]))

    # Stratum metadata (sua_id, upper_ft_val, upper_ft_ref,
    # lower_ft_val, lower_ft_ref).
    stratum_meta = {}
    for row in conn.execute(
            "SELECT STRATUM_ID, SUA_ID, UPPER_FT_VAL, UPPER_FT_REF, "
            "LOWER_FT_VAL, LOWER_FT_REF FROM SUA_STRATUM"):
        stratum_meta[row[0]] = (row[1], row[2], row[3], row[4], row[5])

    # SUA type lookup.
    sua_types = {}
    for sid, stype in conn.execute("SELECT SUA_ID, SUA_TYPE FROM SUA_BASE"):
        sua_types[sid] = stype

    # Circle parts are rendered by SUA_CIRCLE, not as polyline segments.
    circle_parts = set()
    for row in conn.execute("SELECT STRATUM_ID, PART_NUM FROM SUA_CIRCLE"):
        circle_parts.add((row[0], row[1]))

    seg_data = []
    seg_id = 0
    for (stratum_id, part_num), ring in sorted(sua_rings.items()):
        if (stratum_id, part_num) in circle_parts:
            continue
        meta = stratum_meta.get(stratum_id)
        if meta is None:
            continue
        sid, upper_v, upper_r, lower_v, lower_r = meta
        stype = sua_types.get(sid, "")
        for chunk in subdivide_ring(ring):
            seg_id += 1
            for point_seq, (lon, lat) in enumerate(chunk):
                seg_data.append((seg_id, stratum_id, sid, stype,
                                 upper_v, upper_r, lower_v, lower_r,
                                 point_seq, lon, lat))

    conn.executemany(
        "INSERT INTO SUA_SEG VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)", seg_data)
    print(f"  SUA_SEG: {seg_id} segments, {len(seg_data)} points")

    conn.execute("""
        CREATE VIRTUAL TABLE SUA_SEG_RTREE USING rtree(
            id, min_lon, max_lon, min_lat, max_lat
        )
    """)
    conn.execute("""
        INSERT INTO SUA_SEG_RTREE (id, min_lon, max_lon, min_lat, max_lat)
        SELECT SEG_ID,
               MIN(LON_DECIMAL), MAX(LON_DECIMAL),
               MIN(LAT_DECIMAL), MAX(LAT_DECIMAL)
        FROM SUA_SEG
        GROUP BY SEG_ID
    """)

    conn.execute("CREATE INDEX idx_sua_seg ON SUA_SEG(SEG_ID)")


def build_obstacles(conn, dof_zf):
    """Import obstacles from FAA Digital Obstacle File (DOF).

    The DOF ZIP contains fixed-width .Dat files with obstacle records.
    Each file has 4 header lines followed by data rows.
    """
    conn.execute("DROP TABLE IF EXISTS OBS_BASE")
    conn.execute("""
        CREATE TABLE OBS_BASE (
            OAS_NUM TEXT PRIMARY KEY,
            VERIFY_STATUS TEXT,
            COUNTRY TEXT,
            STATE TEXT,
            CITY TEXT,
            LAT_DECIMAL REAL,
            LON_DECIMAL REAL,
            OBSTACLE_TYPE TEXT,
            QUANTITY INTEGER,
            AGL_HT INTEGER,
            AMSL_HT INTEGER,
            LIGHTING TEXT,
            HORIZ_ACC TEXT,
            VERT_ACC TEXT,
            MARKING TEXT,
            FAA_STUDY TEXT,
            ACTION TEXT,
            JDATE TEXT
        )
    """)

    rows = []
    dat_files = sorted(n for n in dof_zf.namelist() if n.endswith('.Dat'))
    for dat_name in dat_files:
        with dof_zf.open(dat_name) as raw:
            lines = io.TextIOWrapper(raw, encoding="latin-1")
            # Skip 4 header lines
            for _ in range(4):
                next(lines, None)
            for line in lines:
                if len(line) < 96:
                    continue
                oas_num = line[0:10].strip()
                verify_status = line[10:12].strip()
                country = line[12:14].strip()
                state = line[14:17].strip()
                city = line[17:35].strip()
                lat_str = line[35:48]
                lon_str = line[48:62]
                obs_type = line[62:81].strip()
                qty_str = line[81:82].strip()
                agl_str = line[82:88].strip()
                amsl_str = line[88:94].strip()
                lighting = line[95:96].strip()

                lat = parse_dof_dms(lat_str)
                lon = parse_dof_dms(lon_str)
                if lat is None or lon is None:
                    continue

                try:
                    quantity = int(qty_str)
                except ValueError:
                    quantity = 1
                try:
                    agl_ht = int(agl_str)
                except ValueError:
                    agl_ht = 0
                try:
                    amsl_ht = int(amsl_str)
                except ValueError:
                    amsl_ht = 0

                # Fields after LIGHTING (may not exist in short records)
                horiz_acc = line[96:98].strip() if len(line) > 98 else ""
                vert_acc = line[98:100].strip() if len(line) > 100 else ""
                marking = line[100:102].strip() if len(line) > 102 else ""
                faa_study = line[102:118].strip() if len(line) > 118 else ""
                action = line[118:120].strip() if len(line) > 120 else ""
                jdate = line[120:128].strip() if len(line) > 128 else ""

                rows.append((oas_num, verify_status, country, state, city,
                             lat, lon, obs_type, quantity, agl_ht, amsl_ht,
                             lighting, horiz_acc, vert_acc, marking,
                             faa_study, action, jdate))

    conn.executemany(
        "INSERT OR IGNORE INTO OBS_BASE VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
        rows,
    )
    print(f"  OBS_BASE: {len(rows)} obstacles")

    conn.execute("""
        CREATE VIRTUAL TABLE OBS_BASE_RTREE USING rtree(
            id, min_lon, max_lon, min_lat, max_lat
        )
    """)
    conn.execute("""
        INSERT INTO OBS_BASE_RTREE (id, min_lon, max_lon, min_lat, max_lat)
        SELECT rowid,
               LON_DECIMAL, LON_DECIMAL,
               LAT_DECIMAL, LAT_DECIMAL
        FROM OBS_BASE
        WHERE LAT_DECIMAL IS NOT NULL AND LON_DECIMAL IS NOT NULL
    """)
    conn.execute("CREATE INDEX idx_obs_state ON OBS_BASE(STATE)")


def build_nav_detail(conn, csv_zf):
    """Import navaid remarks and checkpoints."""
    import_csv(conn, "NAV_RMK", csv_zf, "NAV_RMK.csv", [
        "NAV_ID", "NAV_TYPE", "TAB_NAME", "REF_COL_NAME",
        "REF_COL_SEQ_NO", "REMARK",
    ])
    conn.execute("CREATE INDEX idx_nav_rmk ON NAV_RMK(NAV_ID, NAV_TYPE)")

    import_csv(conn, "NAV_CKPT", csv_zf, "NAV_CKPT.csv", [
        "NAV_ID", "NAV_TYPE", "ALTITUDE", "BRG",
        "AIR_GND_CODE", "CHK_DESC", "ARPT_ID",
    ])
    conn.execute("CREATE INDEX idx_nav_ckpt ON NAV_CKPT(NAV_ID, NAV_TYPE)")


def build_fix_detail(conn, csv_zf):
    """Import fix-to-navaid relationships."""
    import_csv(conn, "FIX_NAV", csv_zf, "FIX_NAV.csv", [
        "FIX_ID", "ICAO_REGION_CODE", "STATE_CODE",
        "NAV_ID", "NAV_TYPE", "BEARING", "DISTANCE",
    ])
    conn.execute("CREATE INDEX idx_fix_nav ON FIX_NAV(FIX_ID)")


def build_awy_base(conn, csv_zf):
    """Import airway metadata."""
    import_csv(conn, "AWY_BASE", csv_zf, "AWY_BASE.csv", [
        "AWY_DESIGNATION", "AWY_LOCATION", "AWY_ID",
        "REMARK", "AIRWAY_STRING",
    ])
    conn.execute("CREATE INDEX idx_awy_base ON AWY_BASE(AWY_ID)")


def build_hpf(conn, csv_zf):
    """Import holding pattern data."""
    import_csv(conn, "HPF_BASE", csv_zf, "HPF_BASE.csv", [
        "HP_NAME", "HP_NO", "STATE_CODE", "COUNTRY_CODE",
        "FIX_ID", "ICAO_REGION_CODE",
        "NAV_ID", "NAV_TYPE",
        "HOLD_DIRECTION", "COURSE_INBOUND_DEG",
        "TURN_DIRECTION", "LEG_LENGTH_DIST",
    ])
    conn.execute("CREATE INDEX idx_hpf_base ON HPF_BASE(FIX_ID)")

    import_csv(conn, "HPF_SPD_ALT", csv_zf, "HPF_SPD_ALT.csv", [
        "HP_NAME", "HP_NO", "SPEED_RANGE", "ALTITUDE",
    ])
    conn.execute("CREATE INDEX idx_hpf_spd ON HPF_SPD_ALT(HP_NAME, HP_NO)")

    import_csv(conn, "HPF_CHRT", csv_zf, "HPF_CHRT.csv", [
        "HP_NAME", "HP_NO", "CHARTING_TYPE_DESC",
    ])
    conn.execute("CREATE INDEX idx_hpf_chrt ON HPF_CHRT(HP_NAME, HP_NO)")

    import_csv(conn, "HPF_RMK", csv_zf, "HPF_RMK.csv", [
        "HP_NAME", "HP_NO", "TAB_NAME", "REF_COL_NAME",
        "REF_COL_SEQ_NO", "REMARK",
    ])
    conn.execute("CREATE INDEX idx_hpf_rmk ON HPF_RMK(HP_NAME, HP_NO)")


def build_maa_rmk(conn, csv_zf):
    """Import MAA remarks."""
    import_csv(conn, "MAA_RMK", csv_zf, "MAA_RMK.csv", [
        "MAA_ID", "TAB_NAME", "REF_COL_NAME",
        "REF_COL_SEQ_NO", "REMARK",
    ])
    conn.execute("CREATE INDEX idx_maa_rmk ON MAA_RMK(MAA_ID)")


def build_mtr_base(conn, csv_zf):
    """Import MTR metadata."""
    import_csv(conn, "MTR_BASE", csv_zf, "MTR_BASE.csv", [
        "ROUTE_TYPE_CODE", "ROUTE_ID", "ARTCC", "FSS", "TIME_OF_USE",
    ])
    conn.execute("CREATE INDEX idx_mtr_base ON MTR_BASE(ROUTE_TYPE_CODE, ROUTE_ID)")


def build_dp(conn, csv_zf):
    """Import departure procedures (SIDs)."""
    import_csv(conn, "DP_BASE", csv_zf, "DP_BASE.csv", [
        "DP_NAME", "AMENDMENT_NO", "ARTCC", "DP_AMEND_EFF_DATE",
        "RNAV_FLAG", "DP_COMPUTER_CODE", "GRAPHICAL_DP_TYPE", "SERVED_ARPT",
    ])
    conn.execute("CREATE INDEX idx_dp_base ON DP_BASE(DP_COMPUTER_CODE)")

    import_csv(conn, "DP_APT", csv_zf, "DP_APT.csv", [
        "DP_NAME", "DP_COMPUTER_CODE", "BODY_NAME", "BODY_SEQ",
        "ARPT_ID", "RWY_END_ID",
    ])
    conn.execute("CREATE INDEX idx_dp_apt ON DP_APT(ARPT_ID)")
    conn.execute("CREATE INDEX idx_dp_apt_code ON DP_APT(DP_COMPUTER_CODE)")

    import_csv(conn, "DP_RTE", csv_zf, "DP_RTE.csv", [
        "DP_COMPUTER_CODE", "ROUTE_PORTION_TYPE", "ROUTE_NAME",
        "BODY_SEQ", "TRANSITION_COMPUTER_CODE", "POINT_SEQ",
        "POINT", "ICAO_REGION_CODE", "POINT_TYPE",
        "NEXT_POINT", "ARPT_RWY_ASSOC",
    ])
    conn.execute("CREATE INDEX idx_dp_rte ON DP_RTE(DP_COMPUTER_CODE)")


def build_star(conn, csv_zf):
    """Import standard terminal arrivals (STARs)."""
    import_csv(conn, "STAR_BASE", csv_zf, "STAR_BASE.csv", [
        "ARRIVAL_NAME", "AMENDMENT_NO", "ARTCC", "STAR_AMEND_EFF_DATE",
        "RNAV_FLAG", "STAR_COMPUTER_CODE", "SERVED_ARPT",
    ])
    conn.execute("CREATE INDEX idx_star_base ON STAR_BASE(STAR_COMPUTER_CODE)")

    import_csv(conn, "STAR_APT", csv_zf, "STAR_APT.csv", [
        "STAR_COMPUTER_CODE", "BODY_NAME", "BODY_SEQ",
        "ARPT_ID", "RWY_END_ID",
    ])
    conn.execute("CREATE INDEX idx_star_apt ON STAR_APT(ARPT_ID)")
    conn.execute("CREATE INDEX idx_star_apt_code ON STAR_APT(STAR_COMPUTER_CODE)")

    import_csv(conn, "STAR_RTE", csv_zf, "STAR_RTE.csv", [
        "STAR_COMPUTER_CODE", "ROUTE_PORTION_TYPE", "ROUTE_NAME",
        "BODY_SEQ", "TRANSITION_COMPUTER_CODE", "POINT_SEQ",
        "POINT", "ICAO_REGION_CODE", "POINT_TYPE",
        "NEXT_POINT", "ARPT_RWY_ASSOC",
    ])
    conn.execute("CREATE INDEX idx_star_rte ON STAR_RTE(STAR_COMPUTER_CODE)")


def build_pfr(conn, csv_zf):
    """Import preferred flight routes and coded departure routes."""
    import_csv(conn, "PFR_BASE", csv_zf, "PFR_BASE.csv", [
        "ORIGIN_ID", "DSTN_ID", "PFR_TYPE_CODE", "ROUTE_NO",
        "SPECIAL_AREA_DESCRIP", "ALT_DESCRIP", "AIRCRAFT", "HOURS",
        "ROUTE_DIR_DESCRIP", "ROUTE_STRING",
    ])
    conn.execute("CREATE INDEX idx_pfr_base ON PFR_BASE(ORIGIN_ID, DSTN_ID)")

    import_csv(conn, "PFR_SEG", csv_zf, "PFR_SEG.csv", [
        "ORIGIN_ID", "DSTN_ID", "PFR_TYPE_CODE", "ROUTE_NO",
        "SEGMENT_SEQ", "SEG_VALUE", "SEG_TYPE",
        "STATE_CODE", "ICAO_REGION_CODE", "NAV_TYPE",
    ])
    conn.execute("CREATE INDEX idx_pfr_seg ON PFR_SEG(ORIGIN_ID, DSTN_ID, ROUTE_NO)")

    import_csv(conn, "CDR", csv_zf, "CDR.csv", [
        "RCode", "Orig", "Dest", "DepFix", "Route String",
        "DCNTR", "ACNTR", "TCNTRs",
    ])
    conn.execute("CREATE INDEX idx_cdr ON CDR(Orig, Dest)")


def build_wxl(conn, csv_zf):
    """Import weather reporting locations."""
    import_csv(conn, "WXL_BASE", csv_zf, "WXL_BASE.csv", [
        "WEA_ID", "CITY", "STATE_CODE", "COUNTRY_CODE",
        "LAT_DECIMAL", "LONG_DECIMAL", "ELEV",
    ])
    conn.execute("CREATE INDEX idx_wxl_base ON WXL_BASE(WEA_ID)")

    # R-tree for spatial queries
    conn.execute("""
        CREATE VIRTUAL TABLE WXL_BASE_RTREE USING rtree(
            id, min_lon, max_lon, min_lat, max_lat
        )
    """)
    conn.execute("""
        INSERT INTO WXL_BASE_RTREE (id, min_lon, max_lon, min_lat, max_lat)
        SELECT rowid,
               CAST(LONG_DECIMAL AS REAL), CAST(LONG_DECIMAL AS REAL),
               CAST(LAT_DECIMAL AS REAL), CAST(LAT_DECIMAL AS REAL)
        FROM WXL_BASE
        WHERE LAT_DECIMAL IS NOT NULL AND LAT_DECIMAL != ''
            AND LONG_DECIMAL IS NOT NULL AND LONG_DECIMAL != ''
    """)

    import_csv(conn, "WXL_SVC", csv_zf, "WXL_SVC.csv", [
        "WEA_ID", "WEA_SVC_TYPE_CODE", "WEA_AFFECT_AREA",
    ])
    conn.execute("CREATE INDEX idx_wxl_svc ON WXL_SVC(WEA_ID)")


def build_frq(conn, csv_zf):
    """Import frequency data for all facility types."""
    import_csv(conn, "FRQ", csv_zf, "FRQ.csv", [
        "FACILITY", "FAC_NAME", "FACILITY_TYPE",
        "ARTCC_OR_FSS_ID",
        "SERVICED_FACILITY", "SERVICED_FAC_NAME", "SERVICED_SITE_TYPE",
        "LAT_DECIMAL", "LONG_DECIMAL",
        "SERVICED_CITY", "SERVICED_STATE", "SERVICED_COUNTRY",
        "TOWER_OR_COMM_CALL", "PRIMARY_APPROACH_RADIO_CALL",
        "FREQ", "SECTORIZATION", "FREQ_USE", "REMARK",
    ])
    conn.execute("CREATE INDEX idx_frq_serviced ON FRQ(SERVICED_FACILITY)")
    conn.execute("CREATE INDEX idx_frq_facility_type ON FRQ(FACILITY_TYPE)")


def build_com(conn, csv_zf):
    """Import communication outlet locations (RCO, RCAG physical sites)."""
    import_csv(conn, "COM", csv_zf, "COM.csv", [
        "COMM_LOC_ID", "COMM_TYPE", "NAV_ID", "NAV_TYPE",
        "CITY", "STATE_CODE", "COUNTRY_CODE",
        "COMM_OUTLET_NAME", "LAT_DECIMAL", "LONG_DECIMAL",
        "FACILITY_ID", "FACILITY_NAME",
        "OPR_HRS", "COMM_STATUS_CODE", "REMARK",
    ])
    conn.execute("CREATE INDEX idx_com_type ON COM(COMM_TYPE)")

    # R-tree for spatial queries (rendering RCO/RCAG sites on map)
    conn.execute("""
        CREATE VIRTUAL TABLE COM_RTREE USING rtree(
            id, min_lon, max_lon, min_lat, max_lat
        )
    """)
    conn.execute("""
        INSERT INTO COM_RTREE (id, min_lon, max_lon, min_lat, max_lat)
        SELECT rowid,
               CAST(LONG_DECIMAL AS REAL), CAST(LONG_DECIMAL AS REAL),
               CAST(LAT_DECIMAL AS REAL), CAST(LAT_DECIMAL AS REAL)
        FROM COM
        WHERE LAT_DECIMAL IS NOT NULL AND LAT_DECIMAL != ''
            AND LONG_DECIMAL IS NOT NULL AND LONG_DECIMAL != ''
    """)


def build_fss(conn, csv_zf):
    """Import Flight Service Stations (renderable map layer)."""
    import_csv(conn, "FSS_BASE", csv_zf, "FSS_BASE.csv", [
        "FSS_ID", "NAME", "FSS_FAC_TYPE", "VOICE_CALL",
        "CITY", "STATE_CODE", "COUNTRY_CODE",
        "LAT_DECIMAL", "LONG_DECIMAL",
        "OPR_HOURS", "FAC_STATUS",
        "PHONE_NO", "TOLL_FREE_NO",
    ])
    conn.execute("CREATE INDEX idx_fss_base_id ON FSS_BASE(FSS_ID)")

    # R-tree spatial index for viewport queries
    conn.execute("""
        CREATE VIRTUAL TABLE FSS_BASE_RTREE USING rtree(
            id, min_lon, max_lon, min_lat, max_lat
        )
    """)
    conn.execute("""
        INSERT INTO FSS_BASE_RTREE (id, min_lon, max_lon, min_lat, max_lat)
        SELECT rowid,
               CAST(LONG_DECIMAL AS REAL), CAST(LONG_DECIMAL AS REAL),
               CAST(LAT_DECIMAL AS REAL), CAST(LAT_DECIMAL AS REAL)
        FROM FSS_BASE
        WHERE LAT_DECIMAL IS NOT NULL AND LONG_DECIMAL IS NOT NULL
            AND LAT_DECIMAL != '' AND LONG_DECIMAL != ''
    """)

    import_csv(conn, "FSS_RMK", csv_zf, "FSS_RMK.csv", [
        "FSS_ID", "NAME", "CITY", "STATE_CODE", "COUNTRY_CODE",
        "REF_COL_NAME", "REF_COL_SEQ_NO", "REMARK",
    ])
    conn.execute("CREATE INDEX idx_fss_rmk_id ON FSS_RMK(FSS_ID)")


def build_awos(conn, csv_zf):
    """Import automated weather stations (renderable map layer)."""
    import_csv(conn, "AWOS", csv_zf, "AWOS.csv", [
        "ASOS_AWOS_ID", "ASOS_AWOS_TYPE",
        "STATE_CODE", "CITY", "COUNTRY_CODE",
        "COMMISSIONED_DATE", "NAVAID_FLAG",
        "LAT_DECIMAL", "LONG_DECIMAL", "ELEV",
        "PHONE_NO", "SECOND_PHONE_NO",
        "SITE_NO", "SITE_TYPE_CODE", "REMARK",
    ])
    conn.execute("CREATE INDEX idx_awos_site_no ON AWOS(SITE_NO)")

    # R-tree spatial index for viewport queries
    conn.execute("""
        CREATE VIRTUAL TABLE AWOS_RTREE USING rtree(
            id, min_lon, max_lon, min_lat, max_lat
        )
    """)
    conn.execute("""
        INSERT INTO AWOS_RTREE (id, min_lon, max_lon, min_lat, max_lat)
        SELECT rowid,
               CAST(LONG_DECIMAL AS REAL), CAST(LONG_DECIMAL AS REAL),
               CAST(LAT_DECIMAL AS REAL), CAST(LAT_DECIMAL AS REAL)
        FROM AWOS
        WHERE LAT_DECIMAL IS NOT NULL AND LONG_DECIMAL IS NOT NULL
            AND LAT_DECIMAL != '' AND LONG_DECIMAL != ''
    """)


def build_adiz(conn, geojson_path):
    """Import ADIZ boundaries from ArcGIS GeoJSON."""
    with open(geojson_path) as f:
        data = json.load(f)

    conn.execute("DROP TABLE IF EXISTS ADIZ_BASE")
    conn.execute("""
        CREATE TABLE ADIZ_BASE (
            ADIZ_ID INTEGER PRIMARY KEY,
            NAME TEXT,
            LOCATION TEXT,
            WORKING_HOURS TEXT,
            MILITARY TEXT
        )
    """)

    conn.execute("DROP TABLE IF EXISTS ADIZ_SHP")
    conn.execute("""
        CREATE TABLE ADIZ_SHP (
            ADIZ_ID INTEGER,
            PART_NUM INTEGER,
            POINT_SEQ INTEGER,
            LON_DECIMAL REAL,
            LAT_DECIMAL REAL
        )
    """)

    adiz_id = 0
    for feature in data.get("features", []):
        props = feature.get("properties", {})
        name = props.get("NAME_TXT", "")
        geom = feature.get("geometry", {})
        geom_type = geom.get("type", "")
        coords = geom.get("coordinates", [])

        # Normalize to list of polygons (each polygon is a list of rings)
        if geom_type == "Polygon":
            polygons = [coords]
        elif geom_type == "MultiPolygon":
            polygons = coords
        else:
            continue

        location = props.get("LOCATIONIND_TXT", "")
        wkhr = props.get("WORKHR_CODE", "")
        mil = props.get("MIL_CODE", "")

        adiz_id += 1
        conn.execute("INSERT INTO ADIZ_BASE VALUES (?, ?, ?, ?, ?)",
                     (adiz_id, name, location, wkhr, mil))

        part_num = 0
        for polygon in polygons:
            for ring in polygon:
                for seq, point in enumerate(ring):
                    lon, lat = point[0], point[1]
                    conn.execute(
                        "INSERT INTO ADIZ_SHP VALUES (?, ?, ?, ?, ?)",
                        (adiz_id, part_num, seq, lon, lat))
                part_num += 1

    count = conn.execute("SELECT COUNT(*) FROM ADIZ_BASE").fetchone()[0]
    shp_count = conn.execute("SELECT COUNT(*) FROM ADIZ_SHP").fetchone()[0]
    print(f"  ADIZ_BASE: {count} zones, ADIZ_SHP: {shp_count} points")

    conn.execute("""
        CREATE VIRTUAL TABLE ADIZ_BASE_RTREE USING rtree(
            id, min_lon, max_lon, min_lat, max_lat
        )
    """)
    conn.execute("""
        INSERT INTO ADIZ_BASE_RTREE (id, min_lon, max_lon, min_lat, max_lat)
        SELECT ADIZ_ID,
               MIN(LON_DECIMAL), MAX(LON_DECIMAL),
               MIN(LAT_DECIMAL), MAX(LAT_DECIMAL)
        FROM ADIZ_SHP
        GROUP BY ADIZ_ID
    """)

    conn.execute("CREATE INDEX idx_adiz_shp ON ADIZ_SHP(ADIZ_ID)")

    # Subdivided segments for rendering (tighter R-tree bboxes)
    conn.execute("DROP TABLE IF EXISTS ADIZ_SEG")
    conn.execute("""
        CREATE TABLE ADIZ_SEG (
            SEG_ID INTEGER,
            ADIZ_ID INTEGER,
            POINT_SEQ INTEGER,
            LON_DECIMAL REAL,
            LAT_DECIMAL REAL
        )
    """)

    # Build lookup from (ADIZ_ID, PART_NUM) to ring points
    adiz_rings = {}
    for row in conn.execute(
            "SELECT ADIZ_ID, PART_NUM, LON_DECIMAL, LAT_DECIMAL "
            "FROM ADIZ_SHP ORDER BY ADIZ_ID, PART_NUM, POINT_SEQ"):
        adiz_rings.setdefault((row[0], row[1]), []).append((row[2], row[3]))

    seg_data = []
    seg_id = 0
    for (aid, part_num), ring in sorted(adiz_rings.items()):
        for chunk in subdivide_ring(ring):
            seg_id += 1
            for point_seq, (lon, lat) in enumerate(chunk):
                seg_data.append((seg_id, aid, point_seq, lon, lat))

    conn.executemany("INSERT INTO ADIZ_SEG VALUES (?, ?, ?, ?, ?)", seg_data)
    print(f"  ADIZ_SEG: {seg_id} segments, {len(seg_data)} points")

    conn.execute("""
        CREATE VIRTUAL TABLE ADIZ_SEG_RTREE USING rtree(
            id, min_lon, max_lon, min_lat, max_lat
        )
    """)
    conn.execute("""
        INSERT INTO ADIZ_SEG_RTREE (id, min_lon, max_lon, min_lat, max_lat)
        SELECT SEG_ID,
               MIN(LON_DECIMAL), MAX(LON_DECIMAL),
               MIN(LAT_DECIMAL), MAX(LAT_DECIMAL)
        FROM ADIZ_SEG
        GROUP BY SEG_ID
    """)

    conn.execute("CREATE INDEX idx_adiz_seg ON ADIZ_SEG(SEG_ID)")


def build_cls_arsp(conn, shp_zf):
    """Import class airspace polygons from ESRI shapefile."""
    shp_name = next((n for n in shp_zf.namelist() if n.endswith('.shp')), None)
    dbf_name = next((n for n in shp_zf.namelist() if n.endswith('.dbf')), None)

    if shp_name is None or dbf_name is None:
        print("  Skipping: Class_Airspace.shp not found in archive")
        return

    # Read attributes and geometries from ZIP
    records = list(read_dbf(io.BytesIO(shp_zf.read(dbf_name))))
    geometries = list(read_shp_polygons(io.BytesIO(shp_zf.read(shp_name))))
    assert len(records) == len(geometries), (
        f"DBF/SHP record count mismatch: {len(records)} vs {len(geometries)}"
    )

    # Create base table
    conn.execute("DROP TABLE IF EXISTS CLS_ARSP_BASE")
    conn.execute("""
        CREATE TABLE CLS_ARSP_BASE (
            ARSP_ID INTEGER PRIMARY KEY,
            NAME TEXT,
            CLASS TEXT,
            LOCAL_TYPE TEXT,
            IDENT TEXT,
            SECTOR TEXT,
            UPPER_DESC TEXT,
            UPPER_VAL TEXT,
            LOWER_DESC TEXT,
            LOWER_VAL TEXT,
            WKHR_CODE TEXT,
            WKHR_RMK TEXT
        )
    """)

    # Create shape table
    conn.execute("DROP TABLE IF EXISTS CLS_ARSP_SHP")
    conn.execute("""
        CREATE TABLE CLS_ARSP_SHP (
            ARSP_ID INTEGER,
            PART_NUM INTEGER,
            IS_HOLE INTEGER,
            POINT_SEQ INTEGER,
            LON_DECIMAL REAL,
            LAT_DECIMAL REAL
        )
    """)

    # Simplify polygons: ~0.0001 degrees ≈ 10 meters, good enough for map display
    epsilon = 0.0001
    total_before = 0
    total_after = 0

    base_rows = []
    shp_rows = []
    for arsp_id, (rec, parts) in enumerate(zip(records, geometries), start=1):
        base_rows.append((
            arsp_id,
            rec.get("NAME", ""),
            rec.get("CLASS", ""),
            rec.get("LOCAL_TYPE", ""),
            rec.get("IDENT", ""),
            rec.get("SECTOR", ""),
            rec.get("UPPER_DESC", ""),
            rec.get("UPPER_VAL", ""),
            rec.get("LOWER_DESC", ""),
            rec.get("LOWER_VAL", ""),
            rec.get("WKHR_CODE", ""),
            rec.get("WKHR_RMK", ""),
        ))
        part_num = 0
        for ring, is_hole in parts:
            hole_flag = 1 if is_hole else 0
            for copy in handle_antimeridian(ring):
                total_before += len(copy)
                simplified = simplify_ring(copy, epsilon)
                total_after += len(simplified)
                for point_seq, (lon, lat) in enumerate(simplified):
                    shp_rows.append((arsp_id, part_num, hole_flag, point_seq, lon, lat))
                part_num += 1

    print(f"  Simplified: {total_before} -> {total_after} points "
          f"({100 * total_after / total_before:.1f}%)")

    conn.executemany(
        "INSERT INTO CLS_ARSP_BASE VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
        base_rows,
    )
    conn.executemany(
        "INSERT INTO CLS_ARSP_SHP VALUES (?, ?, ?, ?, ?, ?)",
        shp_rows,
    )
    print(f"  CLS_ARSP_BASE: {len(base_rows)} airspaces")
    print(f"  CLS_ARSP_SHP: {len(shp_rows)} shape points")

    # R-tree on polygon bounding boxes
    conn.execute("""
        CREATE VIRTUAL TABLE CLS_ARSP_BASE_RTREE USING rtree(
            id, min_lon, max_lon, min_lat, max_lat
        )
    """)
    conn.execute("""
        INSERT INTO CLS_ARSP_BASE_RTREE (id, min_lon, max_lon, min_lat, max_lat)
        SELECT ARSP_ID,
               MIN(LON_DECIMAL), MAX(LON_DECIMAL),
               MIN(LAT_DECIMAL), MAX(LAT_DECIMAL)
        FROM CLS_ARSP_SHP
        GROUP BY ARSP_ID
    """)

    # Index for fast shape point lookup
    conn.execute("CREATE INDEX idx_cls_arsp_shp ON CLS_ARSP_SHP(ARSP_ID)")

    # Subdivided segments for rendering (tighter R-tree bboxes)
    conn.execute("DROP TABLE IF EXISTS CLS_ARSP_SEG")
    conn.execute("""
        CREATE TABLE CLS_ARSP_SEG (
            SEG_ID INTEGER,
            ARSP_ID INTEGER,
            CLASS TEXT,
            LOCAL_TYPE TEXT,
            POINT_SEQ INTEGER,
            LON_DECIMAL REAL,
            LAT_DECIMAL REAL
        )
    """)

    # Build lookup from (ARSP_ID, PART_NUM) to ring points
    arsp_rings = {}
    for row in conn.execute(
            "SELECT ARSP_ID, PART_NUM, LON_DECIMAL, LAT_DECIMAL "
            "FROM CLS_ARSP_SHP ORDER BY ARSP_ID, PART_NUM, POINT_SEQ"):
        arsp_rings.setdefault((row[0], row[1]), []).append((row[2], row[3]))

    # Build lookup from ARSP_ID to (CLASS, LOCAL_TYPE)
    arsp_meta = {}
    for aid, cls, lt in conn.execute(
            "SELECT ARSP_ID, CLASS, LOCAL_TYPE FROM CLS_ARSP_BASE"):
        arsp_meta[aid] = (cls, lt)

    seg_data = []
    seg_id = 0
    for (aid, part_num), ring in sorted(arsp_rings.items()):
        cls, lt = arsp_meta[aid]
        for chunk in subdivide_ring(ring):
            seg_id += 1
            for point_seq, (lon, lat) in enumerate(chunk):
                seg_data.append((seg_id, aid, cls, lt, point_seq, lon, lat))

    conn.executemany("INSERT INTO CLS_ARSP_SEG VALUES (?, ?, ?, ?, ?, ?, ?)", seg_data)
    print(f"  CLS_ARSP_SEG: {seg_id} segments, {len(seg_data)} points")

    conn.execute("""
        CREATE VIRTUAL TABLE CLS_ARSP_SEG_RTREE USING rtree(
            id, min_lon, max_lon, min_lat, max_lat
        )
    """)
    conn.execute("""
        INSERT INTO CLS_ARSP_SEG_RTREE (id, min_lon, max_lon, min_lat, max_lat)
        SELECT SEG_ID,
               MIN(LON_DECIMAL), MAX(LON_DECIMAL),
               MIN(LAT_DECIMAL), MAX(LAT_DECIMAL)
        FROM CLS_ARSP_SEG
        GROUP BY SEG_ID
    """)

    conn.execute("CREATE INDEX idx_cls_arsp_seg ON CLS_ARSP_SEG(SEG_ID)")


def build_artcc(conn, csv_zf):
    """Import ARTCC boundary polygons from ARB_BASE and ARB_SEG CSVs."""
    conn.execute("DROP TABLE IF EXISTS ARTCC_BASE")
    conn.execute("""
        CREATE TABLE ARTCC_BASE (
            ARTCC_ID INTEGER PRIMARY KEY,
            LOCATION_ID TEXT,
            LOCATION_NAME TEXT,
            ALTITUDE TEXT
        )
    """)

    conn.execute("DROP TABLE IF EXISTS ARTCC_SHP")
    conn.execute("""
        CREATE TABLE ARTCC_SHP (
            ARTCC_ID INTEGER,
            POINT_SEQ INTEGER,
            LON_DECIMAL REAL,
            LAT_DECIMAL REAL
        )
    """)

    seg_rows = list(read_csv_rows(csv_zf, "ARB_SEG.csv"))

    # Group segments by LOCATION_ID + ALTITUDE + TYPE, forming one polygon each.
    # TYPE matters for oceanic ARTCCs where a single (LOCATION_ID, ALTITUDE)
    # can have separate CTA, FIR, CTA/FIR, and UTA boundary rings.
    # Within a group, "POINT OF BEGINNING" in BNDRY_PT_DESCRIP delimits
    # sub-rings (e.g. ZOA UTA has 4 separate polygons in one TYPE group).
    from collections import defaultdict
    boundaries = defaultdict(list)
    for row in seg_rows:
        key = (row["LOCATION_ID"], row["ALTITUDE"], row.get("TYPE", ""))
        try:
            seq = int(row["POINT_SEQ"])
            lat = float(row["LAT_DECIMAL"])
            lon = float(row["LONG_DECIMAL"])
            desc = row.get("BNDRY_PT_DESCRIP", "")
            boundaries[key].append((seq, lon, lat, desc))
        except (ValueError, KeyError):
            continue

    base_rows = []
    shp_rows = []

    # Get names from ARB_BASE
    base_info = {}
    for row in read_csv_rows(csv_zf, "ARB_BASE.csv"):
        base_info[row["LOCATION_ID"]] = row.get("LOCATION_NAME", "")

    for (loc_id, altitude, _), points in sorted(boundaries.items()):
        points.sort(key=lambda p: p[0])  # sort by POINT_SEQ
        if len(points) < 3:
            continue

        # Split into sub-rings at "POINT OF BEGINNING" boundaries
        sub_rings = []
        current = []
        for _, lon, lat, desc in points:
            current.append((lon, lat))
            if "POINT OF BEGINNING" in desc.upper():
                sub_rings.append(current)
                current = []
        if current:
            sub_rings.append(current)

        name = base_info.get(loc_id, loc_id)
        for ring in sub_rings:
            if len(ring) < 3:
                continue
            for copy in handle_antimeridian(ring):
                # Close the ring so downstream consumers get a consistent
                # "first == last" invariant (matches class_airspace / SUA).
                if copy[0] != copy[-1]:
                    copy = copy + [copy[0]]
                artcc_id = len(base_rows) + 1
                base_rows.append((artcc_id, loc_id, name, altitude))
                for point_seq, (lon, lat) in enumerate(copy):
                    shp_rows.append((artcc_id, point_seq, lon, lat))

    conn.executemany("INSERT INTO ARTCC_BASE VALUES (?, ?, ?, ?)", base_rows)
    conn.executemany("INSERT INTO ARTCC_SHP VALUES (?, ?, ?, ?)", shp_rows)

    print(f"  ARTCC_BASE: {len(base_rows)} boundaries")
    print(f"  ARTCC_SHP: {len(shp_rows)} shape points")

    # R-tree on bounding boxes
    conn.execute("""
        CREATE VIRTUAL TABLE ARTCC_BASE_RTREE USING rtree(
            id, min_lon, max_lon, min_lat, max_lat
        )
    """)
    conn.execute("""
        INSERT INTO ARTCC_BASE_RTREE (id, min_lon, max_lon, min_lat, max_lat)
        SELECT ARTCC_ID,
               MIN(LON_DECIMAL), MAX(LON_DECIMAL),
               MIN(LAT_DECIMAL), MAX(LAT_DECIMAL)
        FROM ARTCC_SHP
        GROUP BY ARTCC_ID
    """)

    conn.execute("CREATE INDEX idx_artcc_shp ON ARTCC_SHP(ARTCC_ID)")

    # Subdivided segments for rendering (tighter R-tree bboxes)
    conn.execute("DROP TABLE IF EXISTS ARTCC_SEG")
    conn.execute("""
        CREATE TABLE ARTCC_SEG (
            SEG_ID INTEGER,
            ARTCC_ID INTEGER,
            ALTITUDE TEXT,
            POINT_SEQ INTEGER,
            LON_DECIMAL REAL,
            LAT_DECIMAL REAL
        )
    """)

    # Build lookup from ARTCC_ID to its ring points (already in POINT_SEQ order)
    artcc_rings = {}
    for artcc_id, point_seq, lon, lat in shp_rows:
        artcc_rings.setdefault(artcc_id, []).append((lon, lat))

    seg_data = []
    seg_id = 0
    for artcc_id, loc_id, name, altitude in base_rows:
        ring = artcc_rings.get(artcc_id, [])
        for chunk in subdivide_ring(ring):
            seg_id += 1
            for point_seq, (lon, lat) in enumerate(chunk):
                seg_data.append((seg_id, artcc_id, altitude, point_seq, lon, lat))

    conn.executemany("INSERT INTO ARTCC_SEG VALUES (?, ?, ?, ?, ?, ?)", seg_data)
    print(f"  ARTCC_SEG: {seg_id} segments, {len(seg_data)} points")

    conn.execute("""
        CREATE VIRTUAL TABLE ARTCC_SEG_RTREE USING rtree(
            id, min_lon, max_lon, min_lat, max_lat
        )
    """)
    conn.execute("""
        INSERT INTO ARTCC_SEG_RTREE (id, min_lon, max_lon, min_lat, max_lat)
        SELECT SEG_ID,
               MIN(LON_DECIMAL), MAX(LON_DECIMAL),
               MIN(LAT_DECIMAL), MAX(LAT_DECIMAL)
        FROM ARTCC_SEG
        GROUP BY SEG_ID
    """)

    conn.execute("CREATE INDEX idx_artcc_seg ON ARTCC_SEG(SEG_ID)")


def main():
    if len(sys.argv) != 7:
        print(f"Usage: {sys.argv[0]} <csv.zip> <shapefile.zip> <aixm.zip> <dof.zip> <adiz.geojson> <output.db>")
        sys.exit(1)

    csv_zip_path = sys.argv[1]
    shp_zip_path = sys.argv[2]
    aixm_zip_path = sys.argv[3]
    dof_zip_path = sys.argv[4]
    adiz_path = sys.argv[5]
    db_path = sys.argv[6]

    for path in (csv_zip_path, shp_zip_path, aixm_zip_path, dof_zip_path, adiz_path):
        if not os.path.isfile(path):
            print(f"Error: {path} not found")
            sys.exit(1)

    # Remove existing database
    if os.path.exists(db_path):
        os.remove(db_path)

    conn = sqlite3.connect(db_path)
    conn.execute("PRAGMA journal_mode=OFF")
    conn.execute("PRAGMA synchronous=OFF")
    conn.execute("PRAGMA cache_size=-200000")  # 200 MB cache

    print("Building NASR database...")
    with zipfile.ZipFile(csv_zip_path) as csv_zf:
        print("Importing airports...")
        build_apt(conn, csv_zf)

        print("Importing navaids...")
        build_nav(conn, csv_zf)

        print("Importing fixes...")
        build_fix(conn, csv_zf)

        print("Importing airways...")
        build_awy(conn, csv_zf)

        print("Importing parachute jump areas...")
        build_pja(conn, csv_zf)

        print("Importing military training routes...")
        build_mtr(conn, csv_zf)

        print("Importing MOA/SUA...")
        build_maa(conn, csv_zf)

        print("Importing ARTCC boundaries...")
        build_artcc(conn, csv_zf)

        print("Importing runway data...")
        build_apt_rwy(conn, csv_zf)

        print("Importing navaid details...")
        build_nav_detail(conn, csv_zf)

        print("Importing fix details...")
        build_fix_detail(conn, csv_zf)

        print("Importing airway metadata...")
        build_awy_base(conn, csv_zf)

        print("Importing holding patterns...")
        build_hpf(conn, csv_zf)

        print("Importing MAA remarks...")
        build_maa_rmk(conn, csv_zf)

        print("Importing MTR metadata...")
        build_mtr_base(conn, csv_zf)

        print("Importing departure procedures...")
        build_dp(conn, csv_zf)

        print("Importing STARs...")
        build_star(conn, csv_zf)

        print("Importing preferred routes...")
        build_pfr(conn, csv_zf)

        print("Importing weather locations...")
        build_wxl(conn, csv_zf)

        print("Importing airport attendance...")
        build_apt_att(conn, csv_zf)

        print("Importing airport remarks...")
        build_apt_rmk(conn, csv_zf)

        print("Importing ILS data...")
        build_ils(conn, csv_zf)

        print("Importing ATC facilities...")
        build_atc(conn, csv_zf)

        print("Importing frequencies...")
        build_frq(conn, csv_zf)

        print("Importing communication outlets...")
        build_com(conn, csv_zf)

        print("Importing flight service stations...")
        build_fss(conn, csv_zf)

        print("Importing weather stations...")
        build_awos(conn, csv_zf)

    print("Importing class airspace...")
    with zipfile.ZipFile(shp_zip_path) as shp_zf:
        build_cls_arsp(conn, shp_zf)

    print("Importing SUA from AIXM 5.0...")
    with zipfile.ZipFile(aixm_zip_path) as aixm_zf:
        build_sua(conn, aixm_zf)

    print("Importing obstacles from DOF...")
    with zipfile.ZipFile(dof_zip_path) as dof_zf:
        build_obstacles(conn, dof_zf)

    print("Importing ADIZ boundaries...")
    build_adiz(conn, adiz_path)

    conn.commit()

    # Print summary
    print("\nDatabase summary:")
    tables = ["APT_BASE", "CLS_ARSP", "NAV_BASE", "NAV_RMK", "NAV_CKPT",
              "FIX_BASE", "FIX_CHRT", "FIX_NAV",
              "AWY_BASE", "AWY_SEG", "AWY_SEG_ALT",
              "HPF_BASE", "HPF_SPD_ALT", "HPF_CHRT", "HPF_RMK",
              "PJA_BASE", "MTR_BASE", "MTR_SEG",
              "MAA_BASE", "MAA_SHP", "MAA_RMK",
              "DP_BASE", "DP_APT", "DP_RTE",
              "STAR_BASE", "STAR_APT", "STAR_RTE",
              "PFR_BASE", "PFR_SEG", "CDR",
              "WXL_BASE", "WXL_SVC", "WP_LOOKUP",
              "APT_RWY", "APT_RWY_END", "RWY_SEG", "APT_ATT", "APT_RMK",
              "ILS_BASE", "ILS_GS", "ILS_DME", "ILS_MKR", "ILS_RMK",
              "ATC_BASE", "ATC_ATIS", "ATC_RMK", "ATC_SVC",
              "FRQ", "COM", "FSS_BASE", "FSS_RMK", "AWOS",
              "CLS_ARSP_BASE", "CLS_ARSP_SHP", "CLS_ARSP_SEG",
              "SUA_BASE", "SUA_SHP", "SUA_CIRCLE", "SUA_SEG",
              "SUA_FREQ", "SUA_SCHEDULE", "SUA_SERVICE",
              "ARTCC_BASE", "ARTCC_SHP", "ARTCC_SEG", "OBS_BASE",
              "ADIZ_BASE", "ADIZ_SHP", "ADIZ_SEG"]
    for table in tables:
        count = conn.execute(f"SELECT COUNT(*) FROM {table}").fetchone()[0]
        print(f"  {table}: {count} rows")

    conn.close()
    db_size = os.path.getsize(db_path)
    print(f"\nDatabase written to {db_path} ({db_size / 1024 / 1024:.1f} MB)")


if __name__ == "__main__":
    main()
