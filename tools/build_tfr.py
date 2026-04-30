#!/usr/bin/env python3
"""Ingest TFR XNOTAM XML files into the NASR SQLite DB.

Reads every `detail_*.xml` in the given directory.

Usage:
    python tools/build_tfr.py <tfr_dir> <output.db>
"""

import os
import sys
import xml.etree.ElementTree as ET

from build_common import (
    ALT_UNLIMITED_FT, open_output_db, subdivide_ring, write_meta,
)


def _get_text(parent, tag):
    """Return text of a child element, or None if missing."""
    el = parent.find(tag)
    return el.text if el is not None else None


def _parse_xnotam_coord(lat_str, lon_str):
    """Parse XNOTAM coordinate strings like '25.94179199N', '080.86666667W'.

    Returns (lon, lat) as signed floats (west/south negative).
    """
    lat = float(lat_str[:-1])
    if lat_str[-1] == "S":
        lat = -lat
    lon = float(lon_str[:-1])
    if lon_str[-1] == "W":
        lon = -lon
    return lon, lat


def _parse_xnotam_altitude(code_dist, val_dist, uom_dist):
    """Parse XNOTAM altitude fields into (value_ft, ref).

    code_dist: "ALT" (MSL), "HEI" (AGL/SFC), "STD" (flight level)
    val_dist:  numeric string (feet or flight level number)
    uom_dist:  "FT" or "FL"
    """
    if val_dist is None:
        return None, None
    try:
        val = int(val_dist)
    except (ValueError, TypeError):
        return None, None

    if uom_dist == "FL":
        return val * 100, "STD"
    if code_dist == "HEI":
        return val, "SFC"
    if val >= ALT_UNLIMITED_FT:
        return ALT_UNLIMITED_FT, "OTHER"
    return val, "MSL"


def build_tfr(conn, tfr_dir):
    """Import TFR data from XNOTAM XML files."""
    import glob as globmod

    xml_files = sorted(globmod.glob(os.path.join(tfr_dir, "detail_*.xml")))
    if not xml_files:
        print("  No TFR XML files found")
        return

    conn.execute("DROP TABLE IF EXISTS TFR_BASE")
    conn.execute("""
        CREATE TABLE TFR_BASE (
            TFR_ID INTEGER PRIMARY KEY,
            NOTAM_ID TEXT,
            TFR_TYPE TEXT,
            FACILITY TEXT,
            DATE_EFFECTIVE TEXT,
            DATE_EXPIRE TEXT,
            DESCRIPTION TEXT
        )
    """)

    conn.execute("DROP TABLE IF EXISTS TFR_AREA")
    conn.execute("""
        CREATE TABLE TFR_AREA (
            AREA_ID INTEGER PRIMARY KEY,
            TFR_ID INTEGER,
            AREA_NAME TEXT,
            UPPER_FT_VAL INTEGER,
            UPPER_FT_REF TEXT,
            LOWER_FT_VAL INTEGER,
            LOWER_FT_REF TEXT,
            DATE_EFFECTIVE TEXT,
            DATE_EXPIRE TEXT
        )
    """)

    conn.execute("DROP TABLE IF EXISTS TFR_SHP")
    conn.execute("""
        CREATE TABLE TFR_SHP (
            AREA_ID INTEGER,
            POINT_SEQ INTEGER,
            LON_DECIMAL REAL,
            LAT_DECIMAL REAL
        )
    """)

    tfr_id = 0
    area_id = 0
    shp_data = []

    for xml_path in xml_files:
        try:
            tree = ET.parse(xml_path)
        except ET.ParseError as e:
            print(f"  Warning: failed to parse {os.path.basename(xml_path)}: {e}")
            continue

        root = tree.getroot()
        group = root.find("Group")
        if group is None:
            continue

        add = group.find("Add")
        if add is None:
            continue

        notam = add.find("Not")
        if notam is None:
            continue

        # Extract NOTAM identification
        notam_uid = notam.find("NotUid")
        notam_id = ""
        if notam_uid is not None:
            local_name = notam_uid.find("txtLocalName")
            if local_name is not None:
                notam_id = local_name.text or ""

        date_effective = ""
        el = notam.find("dateEffective")
        if el is not None:
            date_effective = el.text or ""

        date_expire = ""
        el = notam.find("dateExpire")
        if el is not None:
            date_expire = el.text or ""

        # TFR type from the TfrNot section
        tfr_not = notam.find("TfrNot")
        tfr_type = ""
        if tfr_not is not None:
            el = tfr_not.find("codeType")
            if el is not None:
                tfr_type = el.text or ""

        facility = ""
        el = notam.find("codeFacility")
        if el is not None:
            facility = el.text or ""

        # Build description from NOTAM text
        description = ""
        el = notam.find("txtDescrUSNS")
        if el is not None:
            description = el.text or ""

        tfr_id += 1
        conn.execute("INSERT INTO TFR_BASE VALUES (?, ?, ?, ?, ?, ?, ?)",
                     (tfr_id, notam_id, tfr_type, facility,
                      date_effective, date_expire, description))

        # Parse TFRAreaGroups
        if tfr_not is None:
            continue

        for area_group in tfr_not.findall("TFRAreaGroup"):
            ase = area_group.find("aseTFRArea")
            if ase is None:
                continue

            area_name = ""
            el = ase.find("txtName")
            if el is not None:
                area_name = el.text or ""

            # Altitude
            upper_code = _get_text(ase, "codeDistVerUpper")
            upper_val = _get_text(ase, "valDistVerUpper")
            upper_uom = _get_text(ase, "uomDistVerUpper")
            lower_code = _get_text(ase, "codeDistVerLower")
            lower_val = _get_text(ase, "valDistVerLower")
            lower_uom = _get_text(ase, "uomDistVerLower")

            upper_ft, upper_ref = _parse_xnotam_altitude(
                upper_code, upper_val, upper_uom)
            lower_ft, lower_ref = _parse_xnotam_altitude(
                lower_code, lower_val, lower_uom)

            # Per-area schedule (may differ from NOTAM-level dates)
            area_effective = ""
            area_expire = ""
            sched = ase.find("ScheduleGroup")
            if sched is not None:
                el = sched.find("dateEffective")
                if el is not None:
                    area_effective = el.text or ""
                el = sched.find("dateExpire")
                if el is not None:
                    area_expire = el.text or ""

            area_id += 1
            conn.execute(
                "INSERT INTO TFR_AREA VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
                (area_id, tfr_id, area_name,
                 upper_ft, upper_ref, lower_ft, lower_ref,
                 area_effective, area_expire))

            # Parse pre-tessellated polygon from abdMergedArea
            merged = area_group.find("abdMergedArea")
            if merged is None:
                continue

            points = []
            for avx in merged.findall("Avx"):
                lat_el = avx.find("geoLat")
                lon_el = avx.find("geoLong")
                if lat_el is None or lon_el is None:
                    continue
                try:
                    lon, lat = _parse_xnotam_coord(lat_el.text, lon_el.text)
                    points.append((lon, lat))
                except (ValueError, TypeError, IndexError):
                    continue

            if len(points) < 3:
                continue

            for seq, (lon, lat) in enumerate(points):
                shp_data.append((area_id, seq, lon, lat))

    conn.executemany("INSERT INTO TFR_SHP VALUES (?, ?, ?, ?)", shp_data)

    print(f"  TFR_BASE: {tfr_id} TFRs, TFR_AREA: {area_id} areas, "
          f"TFR_SHP: {len(shp_data)} points")

    # Indexes
    conn.execute("CREATE INDEX idx_tfr_area ON TFR_AREA(TFR_ID)")
    conn.execute("CREATE INDEX idx_tfr_shp ON TFR_SHP(AREA_ID)")

    conn.execute("""
        CREATE VIRTUAL TABLE TFR_AREA_RTREE USING rtree(
            id, min_lon, max_lon, min_lat, max_lat
        )
    """)
    conn.execute("""
        INSERT INTO TFR_AREA_RTREE (id, min_lon, max_lon, min_lat, max_lat)
        SELECT AREA_ID,
               MIN(LON_DECIMAL), MAX(LON_DECIMAL),
               MIN(LAT_DECIMAL), MAX(LAT_DECIMAL)
        FROM TFR_SHP
        GROUP BY AREA_ID
    """)

    # Subdivided segments for rendering
    conn.execute("DROP TABLE IF EXISTS TFR_SEG")
    conn.execute("""
        CREATE TABLE TFR_SEG (
            SEG_ID INTEGER,
            AREA_ID INTEGER,
            TFR_ID INTEGER,
            UPPER_FT_VAL INTEGER,
            UPPER_FT_REF TEXT,
            LOWER_FT_VAL INTEGER,
            LOWER_FT_REF TEXT,
            POINT_SEQ INTEGER,
            LON_DECIMAL REAL,
            LAT_DECIMAL REAL
        )
    """)

    # Build ring lookup from TFR_SHP
    tfr_rings = {}
    for row in conn.execute(
            "SELECT AREA_ID, LON_DECIMAL, LAT_DECIMAL "
            "FROM TFR_SHP ORDER BY AREA_ID, POINT_SEQ"):
        tfr_rings.setdefault(row[0], []).append((row[1], row[2]))

    # Area metadata lookup
    area_meta = {}
    for row in conn.execute(
            "SELECT AREA_ID, TFR_ID, UPPER_FT_VAL, UPPER_FT_REF, "
            "LOWER_FT_VAL, LOWER_FT_REF FROM TFR_AREA"):
        area_meta[row[0]] = (row[1], row[2], row[3], row[4], row[5])

    seg_data = []
    seg_id = 0
    for aid, ring in sorted(tfr_rings.items()):
        meta = area_meta.get(aid)
        if meta is None:
            continue
        tid, upper_v, upper_r, lower_v, lower_r = meta
        for chunk in subdivide_ring(ring):
            seg_id += 1
            for point_seq, (lon, lat) in enumerate(chunk):
                seg_data.append((seg_id, aid, tid,
                                 upper_v, upper_r, lower_v, lower_r,
                                 point_seq, lon, lat))

    conn.executemany(
        "INSERT INTO TFR_SEG VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)", seg_data)
    print(f"  TFR_SEG: {seg_id} segments, {len(seg_data)} points")

    conn.execute("""
        CREATE VIRTUAL TABLE TFR_SEG_RTREE USING rtree(
            id, min_lon, max_lon, min_lat, max_lat
        )
    """)
    conn.execute("""
        INSERT INTO TFR_SEG_RTREE (id, min_lon, max_lon, min_lat, max_lat)
        SELECT SEG_ID,
               MIN(LON_DECIMAL), MAX(LON_DECIMAL),
               MIN(LAT_DECIMAL), MAX(LAT_DECIMAL)
        FROM TFR_SEG
        GROUP BY SEG_ID
    """)

    conn.execute("CREATE INDEX idx_tfr_seg ON TFR_SEG(SEG_ID)")

    # TFRs are inherently ephemeral. Until the in-app fetcher lands
    # (Stage 3), they're snapshots of whatever was on tfr.faa.gov when
    # download_all.py ran; the registry shows this as `unknown` until
    # the in-app source replaces it with a runtime-tracked freshness.
    write_meta(conn, "tfr", kind="ephemeral",
               info="TFR snapshot (ephemeral)")


def main():
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <tfr_dir> <output.db>")
        sys.exit(1)
    tfr_dir, db_path = sys.argv[1], sys.argv[2]

    conn = open_output_db(db_path)
    print("Importing TFRs...")
    build_tfr(conn, tfr_dir)
    conn.commit()
    conn.close()


if __name__ == "__main__":
    main()
