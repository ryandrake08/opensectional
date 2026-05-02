#!/usr/bin/env python3
"""Ingest the 28-day FAA NASR CSV distribution into the SQLite DB.

Covers airports, navaids, fixes, airways, ARTCC boundaries, MTRs,
MAAs, PJAs, runway/ILS/ATC detail, procedures (SIDs/STARs/PFRs),
weather/comm/fss stations, and related remark/chart tables.

Usage:
    python tools/build_nasr.py <csv.zip> <output.db>
"""

import csv
import datetime
import io
import math
import re
import sys
import zipfile
from collections import defaultdict

from build_common import (
    _parse_date_loose, handle_antimeridian, normalize_date_column,
    open_output_db, subdivide_ring, write_meta,
)


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
    normalize_date_column(conn, "APT_BASE", "ACTIVATION_DATE")

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

    # Adjacent-fix lookup used by flight_route airway-ize. The two
    # indexes let SQLite's OR-term optimizer resolve each branch of
    # `(FROM_POINT=?1 AND TO_POINT=?2) OR (FROM_POINT=?2 AND TO_POINT=?1)`
    # with a direct index seek instead of a full table scan.
    conn.execute("CREATE INDEX idx_awy_seg_from_to ON AWY_SEG(FROM_POINT, TO_POINT)")
    conn.execute("CREATE INDEX idx_awy_seg_to_from ON AWY_SEG(TO_POINT, FROM_POINT)")


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

    # Import shape points and convert DMS to decimal. Rows whose DMS
    # cannot be parsed are skipped entirely rather than carrying a (0,0)
    # placeholder, so any (lat, lon) in MAA_SHP is real shape data.
    import_csv(conn, "MAA_SHP_RAW", csv_zf, "MAA_SHP.csv", [
        "MAA_ID", "POINT_SEQ", "LATITUDE", "LONGITUDE",
    ])

    conn.execute("""
        CREATE TABLE MAA_SHP (
            MAA_ID TEXT,
            POINT_SEQ INTEGER,
            LAT_DECIMAL REAL,
            LON_DECIMAL REAL
        )
    """)
    inserts = []
    for maa_id, seq, lat_dms, lon_dms in conn.execute(
            "SELECT MAA_ID, CAST(POINT_SEQ AS INTEGER), LATITUDE, LONGITUDE "
            "FROM MAA_SHP_RAW"):
        lat = parse_dms(lat_dms)
        lon = parse_dms(lon_dms)
        if lat is not None and lon is not None:
            inserts.append((maa_id, seq, lat, lon))
    conn.executemany(
        "INSERT INTO MAA_SHP (MAA_ID, POINT_SEQ, LAT_DECIMAL, LON_DECIMAL) "
        "VALUES (?, ?, ?, ?)", inserts)
    conn.execute("DROP TABLE MAA_SHP_RAW")
    print(f"  MAA_SHP: {len(inserts)} shape points converted")

    # Reorder shape points into convex hull winding order.
    # FAA data stores quad vertices in arbitrary order (often as opposite
    # corner pairs), causing bowtie rendering artifacts.
    rows = conn.execute(
        "SELECT MAA_ID, POINT_SEQ, LAT_DECIMAL, LON_DECIMAL FROM MAA_SHP "
        "ORDER BY MAA_ID, POINT_SEQ").fetchall()

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

    # Close each ring by appending the first point at the end so
    # downstream renderers don't need a special "close" flag.
    close_inserts = []
    for maa_id, pts in by_id.items():
        if len(pts) < 3:
            continue
        max_seq = len(pts)
        first = conn.execute(
            "SELECT LAT_DECIMAL, LON_DECIMAL FROM MAA_SHP "
            "WHERE MAA_ID = ? ORDER BY POINT_SEQ LIMIT 1",
            (maa_id,)).fetchone()
        last = conn.execute(
            "SELECT LAT_DECIMAL, LON_DECIMAL FROM MAA_SHP "
            "WHERE MAA_ID = ? ORDER BY POINT_SEQ DESC LIMIT 1",
            (maa_id,)).fetchone()
        if first and last and (first[0] != last[0] or first[1] != last[1]):
            close_inserts.append((maa_id, max_seq + 1, first[0], first[1]))

    if close_inserts:
        conn.executemany(
            "INSERT INTO MAA_SHP (MAA_ID, POINT_SEQ, LAT_DECIMAL, LON_DECIMAL) "
            "VALUES (?, ?, ?, ?)",
            close_inserts)
        print(f"  MAA_SHP: closed {len(close_inserts)} rings")

    conn.execute("CREATE INDEX idx_maa_shp ON MAA_SHP(MAA_ID)")

    # Parse MAA altitude strings like "5000AGL" → (5000, "AGL")
    def _parse_maa_alt(s):
        if not s or not s.strip():
            return 0, ""
        s = s.strip()
        if s.endswith("AGL"):
            return int(s[:-3]), "AGL"
        if s.endswith("MSL"):
            return int(s[:-3]), "MSL"
        return 0, ""

    # Build MAA_BASE with parsed coordinates and altitudes. LAT/LON are
    # NULL for shape-defined MAAs (no center point); the C++ side detects
    # shape MAAs by joining to MAA_SHP rather than by checking a coord
    # sentinel.
    conn.execute("""
        CREATE TABLE MAA_BASE AS
        SELECT
            MAA_ID,
            MAA_TYPE_NAME AS TYPE,
            MAA_NAME AS NAME,
            LATITUDE AS LAT_DMS,
            LONGITUDE AS LON_DMS,
            CAST(NULL AS REAL) AS LAT,
            CAST(NULL AS REAL) AS LON,
            CASE WHEN MAA_RADIUS IS NOT NULL AND TRIM(MAA_RADIUS) != ''
                 THEN CAST(MAA_RADIUS AS REAL) ELSE 0.0 END AS RADIUS_NM,
            0 AS MAX_ALT_FT,
            '' AS MAX_ALT_REF,
            0 AS MIN_ALT_FT,
            '' AS MIN_ALT_REF
        FROM MAA_RAW
    """)

    alt_updates = []
    for row in conn.execute(
            "SELECT rowid, MAX_ALT, MIN_ALT FROM MAA_RAW"):
        max_ft, max_ref = _parse_maa_alt(row[1])
        min_ft, min_ref = _parse_maa_alt(row[2])
        alt_updates.append((max_ft, max_ref, min_ft, min_ref, row[0]))
    conn.executemany(
        "UPDATE MAA_BASE SET MAX_ALT_FT=?, MAX_ALT_REF=?, "
        "MIN_ALT_FT=?, MIN_ALT_REF=? WHERE rowid=?",
        alt_updates)
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
    pts = conn.execute("SELECT COUNT(*) FROM MAA_BASE WHERE LAT IS NOT NULL").fetchone()[0]
    shps = total - pts
    print(f"  MAA_BASE: {total} areas ({pts} point/radius, {shps} shape-defined)")

    # R-tree: point-based MAAs use their coordinates; shape-based MAAs
    # (LAT IS NULL) use the bounding box of their MAA_SHP polygon.
    conn.execute("""
        CREATE VIRTUAL TABLE MAA_BASE_RTREE USING rtree(
            id, min_lon, max_lon, min_lat, max_lat
        )
    """)
    maa_point_rows = conn.execute(
        "SELECT rowid, LON, LAT, RADIUS_NM FROM MAA_BASE WHERE LAT IS NOT NULL"
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
        WHERE b.LAT IS NULL
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
    normalize_date_column(conn, "DP_BASE", "DP_AMEND_EFF_DATE")
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
    normalize_date_column(conn, "STAR_BASE", "STAR_AMEND_EFF_DATE")
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
    normalize_date_column(conn, "AWOS", "COMMISSIONED_DATE")
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


def build_artcc(conn, csv_zf):
    """Import ARTCC boundary polygons from ARB_BASE and ARB_SEG CSVs.

    Natural keys per FAA NASR spec:
      ARB_BASE  — LOCATION_ID uniquely identifies a center.
      ARB_SEG   — REC_ID (concat of LOCATION_ID, BNDRY_CODE, point designator)
                  uniquely identifies a boundary point.

    Our polygon rows carry per-location ARB_BASE fields (ICAO_ID,
    LOCATION_TYPE, CITY, STATE, COUNTRY_CODE, CROSS_REF) denormalised
    across the 1..N polygons that share a LOCATION_ID.
    """
    conn.execute("DROP TABLE IF EXISTS ARTCC_BASE")
    conn.execute("""
        CREATE TABLE ARTCC_BASE (
            ARTCC_ID INTEGER PRIMARY KEY,
            LOCATION_ID TEXT,
            LOCATION_NAME TEXT,
            ALTITUDE TEXT,
            TYPE TEXT,
            ICAO_ID TEXT,
            LOCATION_TYPE TEXT,
            CITY TEXT,
            STATE TEXT,
            COUNTRY_CODE TEXT,
            CROSS_REF TEXT
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

    # Per-location metadata from ARB_BASE, keyed on LOCATION_ID (unique there).
    base_info = {}
    for row in read_csv_rows(csv_zf, "ARB_BASE.csv"):
        base_info[row["LOCATION_ID"]] = row

    for (loc_id, altitude, type_), points in sorted(boundaries.items()):
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

        info = base_info.get(loc_id)
        if info is None:
            # ARB_SEG references a LOCATION_ID that's not in ARB_BASE. All 26
            # US-controlled centres in ARB_SEG are present in ARB_BASE, so this
            # indicates a NASR schema change worth surfacing rather than masking.
            raise KeyError(f"ARB_SEG LOCATION_ID {loc_id!r} not found in ARB_BASE")
        name = info.get("LOCATION_NAME", "")
        icao_id = info.get("ICAO_ID", "")
        location_type = info.get("LOCATION_TYPE", "")
        city = info.get("CITY", "")
        state = info.get("STATE", "")
        country_code = info.get("COUNTRY_CODE", "")
        cross_ref = info.get("CROSS_REF", "")

        for ring in sub_rings:
            if len(ring) < 3:
                continue
            for copy in handle_antimeridian(ring):
                # Close the ring so downstream consumers get a consistent
                # "first == last" invariant (matches class_airspace / SUA).
                if copy[0] != copy[-1]:
                    copy = copy + [copy[0]]
                artcc_id = len(base_rows) + 1
                base_rows.append((artcc_id, loc_id, name, altitude, type_,
                                  icao_id, location_type, city, state,
                                  country_code, cross_ref))
                for point_seq, (lon, lat) in enumerate(copy):
                    shp_rows.append((artcc_id, point_seq, lon, lat))

    conn.executemany(
        "INSERT INTO ARTCC_BASE VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
        base_rows)
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
            TYPE TEXT,
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
    for row in base_rows:
        artcc_id, _loc_id, _name, altitude, type_ = row[:5]
        ring = artcc_rings.get(artcc_id, [])
        for chunk in subdivide_ring(ring):
            seg_id += 1
            for point_seq, (lon, lat) in enumerate(chunk):
                seg_data.append((seg_id, artcc_id, altitude, type_, point_seq, lon, lat))

    conn.executemany("INSERT INTO ARTCC_SEG VALUES (?, ?, ?, ?, ?, ?, ?)", seg_data)
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


def build_all_nasr(conn, csv_zf):
    """Run every NASR CSV builder in dependency order on an open connection."""
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

    write_nasr_meta(conn, csv_zf)


def write_nasr_meta(conn, csv_zf):
    """Extract the cycle effective date from any CSV (first data row's
    EFF_DATE column) and record a META entry. NASR runs on a 28-day
    cycle, so the next cycle's effective date is also the current
    cycle's expiration."""
    eff_iso = None
    try:
        with csv_zf.open("APT_BASE.csv") as f:
            reader = csv.DictReader(io.TextIOWrapper(f, encoding="utf-8"))
            row = next(reader, None)
            if row is not None:
                eff_iso = _parse_date_loose(row.get("EFF_DATE"))
    except KeyError:
        pass

    expires_iso = None
    info = "NASR CSV"
    if eff_iso is not None:
        eff = datetime.date.fromisoformat(eff_iso)
        expires_iso = (eff + datetime.timedelta(days=28)).isoformat()
        info = f"NASR cycle {eff.strftime('%d %b %Y')}"

    write_meta(conn, "nasr",
               effective=eff_iso, expires=expires_iso, info=info)


def main():
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <csv.zip> <output.db>")
        sys.exit(1)
    csv_zip_path, db_path = sys.argv[1], sys.argv[2]

    conn = open_output_db(db_path)
    with zipfile.ZipFile(csv_zip_path) as csv_zf:
        build_all_nasr(conn, csv_zf)
    conn.commit()
    conn.close()


if __name__ == "__main__":
    main()
