#!/usr/bin/env python3
"""Build SQLite database from FAA NASR data.

Imports CSV tables and ESRI shapefiles, adds decimal coordinates where
needed, and creates R-tree spatial indexes for bounding box queries.

Usage:
    python3 build_nasr_db.py <csv_dir> <shapefile_dir> <output.db>

Example:
    python3 build_nasr_db.py nasr_data/19_Feb_2026_CSV nasr_data/Shape_Files nasr.db
"""

import csv
import math
import os
import re
import sqlite3
import struct
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
        all_cols = reader.fieldnames or []
        if columns is None:
            columns = list(all_cols)

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



def simplify_ring(points, epsilon):
    """Simplify a polygon ring using Ramer-Douglas-Peucker algorithm.

    Points are (lon, lat) tuples. Epsilon is in degrees.
    """
    if len(points) <= 3:
        return points

    # Find the point farthest from the line between start and end
    start = points[0]
    end = points[-1]
    max_dist = 0.0
    max_idx = 0
    dx = end[0] - start[0]
    dy = end[1] - start[1]
    line_len_sq = dx * dx + dy * dy

    for i in range(1, len(points) - 1):
        px = points[i][0] - start[0]
        py = points[i][1] - start[1]
        if line_len_sq > 0:
            # Perpendicular distance from point to line
            dist = abs(px * dy - py * dx) / math.sqrt(line_len_sq)
        else:
            dist = math.sqrt(px * px + py * py)
        if dist > max_dist:
            max_dist = dist
            max_idx = i

    if max_dist > epsilon:
        left = simplify_ring(points[:max_idx + 1], epsilon)
        right = simplify_ring(points[max_idx:], epsilon)
        return left[:-1] + right
    else:
        return [start, end]


def read_dbf(path):
    """Read a .dbf file, yielding dicts of field_name -> string value."""
    with open(path, "rb") as f:
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


def read_shp_polygons(path):
    """Read polygon geometries from a .shp file.

    Yields lists of parts, where each part is a list of (lon, lat) tuples.
    Handles both Polygon (type 5) and PolygonZ (type 15).
    """
    with open(path, "rb") as f:
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
                ring = [(all_points[j * 2], all_points[j * 2 + 1])
                        for j in range(start, end)]
                parts.append(ring)

            yield parts


def build_cls_arsp(conn, shp_dir):
    """Import class airspace polygons from ESRI shapefile."""
    shp_path = os.path.join(shp_dir, "Class_Airspace.shp")
    dbf_path = os.path.join(shp_dir, "Class_Airspace.dbf")

    if not os.path.exists(shp_path):
        print("  Skipping: Class_Airspace.shp not found")
        return

    # Read attributes and geometries in parallel
    records = list(read_dbf(dbf_path))
    geometries = list(read_shp_polygons(shp_path))
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
            UPPER_DESC TEXT,
            UPPER_VAL TEXT,
            LOWER_DESC TEXT,
            LOWER_VAL TEXT
        )
    """)

    # Create shape table
    conn.execute("DROP TABLE IF EXISTS CLS_ARSP_SHP")
    conn.execute("""
        CREATE TABLE CLS_ARSP_SHP (
            ARSP_ID INTEGER,
            PART_NUM INTEGER,
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
            rec.get("UPPER_DESC", ""),
            rec.get("UPPER_VAL", ""),
            rec.get("LOWER_DESC", ""),
            rec.get("LOWER_VAL", ""),
        ))
        for part_num, ring in enumerate(parts):
            total_before += len(ring)
            simplified = simplify_ring(ring, epsilon)
            total_after += len(simplified)
            for point_seq, (lon, lat) in enumerate(simplified):
                shp_rows.append((arsp_id, part_num, point_seq, lon, lat))

    print(f"  Simplified: {total_before} -> {total_after} points "
          f"({100 * total_after / total_before:.1f}%)")

    conn.executemany(
        "INSERT INTO CLS_ARSP_BASE VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
        base_rows,
    )
    conn.executemany(
        "INSERT INTO CLS_ARSP_SHP VALUES (?, ?, ?, ?, ?)",
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


def main():
    if len(sys.argv) != 4:
        print(f"Usage: {sys.argv[0]} <csv_dir> <shapefile_dir> <output.db>")
        sys.exit(1)

    csv_dir = sys.argv[1]
    shp_dir = sys.argv[2]
    db_path = sys.argv[3]

    if not os.path.isdir(csv_dir):
        print(f"Error: {csv_dir} is not a directory")
        sys.exit(1)
    if not os.path.isdir(shp_dir):
        print(f"Error: {shp_dir} is not a directory")
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

    print("Importing class airspace...")
    build_cls_arsp(conn, shp_dir)

    conn.commit()

    # Print summary
    print("\nDatabase summary:")
    tables = ["APT_BASE", "NAV_BASE", "FIX_BASE", "AWY_SEG",
              "MAA_BASE", "MAA_SHP", "APT_RWY", "APT_RWY_END",
              "CLS_ARSP_BASE", "CLS_ARSP_SHP"]
    for table in tables:
        count = conn.execute(f"SELECT COUNT(*) FROM {table}").fetchone()[0]
        print(f"  {table}: {count} rows")

    conn.close()
    db_size = os.path.getsize(db_path)
    print(f"\nDatabase written to {db_path} ({db_size / 1024 / 1024:.1f} MB)")


if __name__ == "__main__":
    main()
