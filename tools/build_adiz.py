#!/usr/bin/env python3
"""Ingest ADIZ boundaries (ArcGIS GeoJSON) into the NASR SQLite DB.

Usage:
    python tools/build_adiz.py <adiz.geojson> <output.db>
"""

import datetime
import json
import os
import sys

from build_common import open_output_db, subdivide_ring, write_meta


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

    conn.execute("DROP TABLE IF EXISTS ADIZ_BASE_RTREE")
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

    conn.execute("DROP TABLE IF EXISTS ADIZ_SEG_RTREE")
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

    # ADIZ has no published cycle. Record the GeoJSON file's mtime as
    # the effective date and leave expiration unset — the C++ side
    # treats this as a never-expiring source (always shown as fresh
    # provided the row exists).
    try:
        mtime = datetime.datetime.fromtimestamp(
            os.path.getmtime(geojson_path), tz=datetime.timezone.utc)
        eff_iso = mtime.date().isoformat()
        info_text = f"ADIZ {mtime.strftime('%d %b %Y')}"
    except OSError:
        eff_iso = None
        info_text = "ADIZ"
    write_meta(conn, "adiz", effective=eff_iso, info=info_text)


def main():
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <adiz.geojson> <output.db>")
        sys.exit(1)
    geojson_path, db_path = sys.argv[1], sys.argv[2]

    conn = open_output_db(db_path)
    print("Importing ADIZ boundaries...")
    build_adiz(conn, geojson_path)
    conn.commit()
    conn.close()


if __name__ == "__main__":
    main()
