#!/usr/bin/env python3
"""Ingest the FAA class airspace ESRI shapefile into the NASR SQLite DB.

Usage:
    python tools/build_shp.py <shapefile.zip> <output.db>
"""

import datetime
import io
import struct
import sys
import zipfile

from build_common import (
    handle_antimeridian, open_output_db, simplify_ring, subdivide_ring,
    write_meta,
)


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
    def _parse_cls_arsp_alt(desc, val, uom, code):
        """Parse class airspace altitude fields into (ft, ref).

        Fields from the shapefile:
          DESC: AA (And Above), TI (To Including), TNI (To Not Including),
                ANI (Above Not Including), or blank
          VAL:  altitude in feet, or -9998 for undefined
          UOM:  FT or blank
          CODE: MSL, SFC, STD, UNLTD, or blank

        CODE is the authoritative reference. DESC=AA with blank
        CODE/UOM and VAL=-9998 means "up to overlying airspace"
        (typically 18000 MSL for Class E).
        """
        if not val or not val.strip():
            return 0, ""
        try:
            ft = int(val)
        except ValueError:
            return 0, ""
        if ft < 0:
            return 0, ""
        if code == "SFC" or (ft == 0 and not code):
            return 0, "SFC"
        if code == "MSL" or code == "STD":
            return ft, "MSL"
        if code == "UNLTD":
            return 0, ""
        # Blank code: infer from context.  All class airspace altitudes
        # are MSL except Class E AA entries with -9998 (already filtered
        # above as ft < 0).
        if ft > 0:
            return ft, "MSL"
        return 0, ""

    conn.execute("DROP TABLE IF EXISTS CLS_ARSP_BASE")
    conn.execute("""
        CREATE TABLE CLS_ARSP_BASE (
            ARSP_ID INTEGER PRIMARY KEY,
            NAME TEXT,
            CLASS TEXT,
            LOCAL_TYPE TEXT,
            IDENT TEXT,
            SECTOR TEXT,
            UPPER_FT INTEGER,
            UPPER_REF TEXT,
            LOWER_FT INTEGER,
            LOWER_REF TEXT,
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
        upper_ft, upper_ref = _parse_cls_arsp_alt(
            rec.get("UPPER_DESC", ""), rec.get("UPPER_VAL", ""),
            rec.get("UPPER_UOM", ""), rec.get("UPPER_CODE", ""))
        lower_ft, lower_ref = _parse_cls_arsp_alt(
            rec.get("LOWER_DESC", ""), rec.get("LOWER_VAL", ""),
            rec.get("LOWER_UOM", ""), rec.get("LOWER_CODE", ""))
        base_rows.append((
            arsp_id,
            rec.get("NAME", ""),
            rec.get("CLASS", ""),
            rec.get("LOCAL_TYPE", ""),
            rec.get("IDENT", ""),
            rec.get("SECTOR", ""),
            upper_ft,
            upper_ref,
            lower_ft,
            lower_ref,
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

    write_shp_meta(conn, shp_zf)


def main():
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <shapefile.zip> <output.db>")
        sys.exit(1)
    shp_zip_path, db_path = sys.argv[1], sys.argv[2]

    conn = open_output_db(db_path)
    print("Importing class airspace...")
    with zipfile.ZipFile(shp_zip_path) as shp_zf:
        build_cls_arsp(conn, shp_zf)
    conn.commit()
    conn.close()


def write_shp_meta(conn, shp_zf):
    """The class airspace shapefile carries no EFF_DATE column, but the
    FAA stamps the inner .dbf with its build date. Use that as the
    effective date and the NASR 28-day cycle for expiration."""
    eff_iso = None
    dbf_name = next((n for n in shp_zf.namelist() if n.endswith('.dbf')), None)
    if dbf_name is not None:
        # ZipInfo.date_time is a 6-tuple (Y,M,D,h,m,s) in local time;
        # treat as UTC date — close enough for cycle math.
        info = shp_zf.getinfo(dbf_name)
        try:
            eff_iso = datetime.date(*info.date_time[:3]).isoformat()
        except ValueError:
            eff_iso = None

    expires_iso = None
    info_text = "Class airspace shapefile"
    if eff_iso is not None:
        eff = datetime.date.fromisoformat(eff_iso)
        expires_iso = (eff + datetime.timedelta(days=28)).isoformat()
        info_text = f"Class airspace SHP {eff.strftime('%d %b %Y')}"

    write_meta(conn, "shp",
               effective=eff_iso, expires=expires_iso, info=info_text)


if __name__ == "__main__":
    main()
