#!/usr/bin/env python3
"""Ingest FAA Digital Obstacle File (DOF) into the NASR SQLite DB.

Usage:
    python tools/build_dof.py <dof.zip> <output.db>
"""

import io
import sys
import zipfile

from build_common import open_output_db


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


def main():
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <dof.zip> <output.db>")
        sys.exit(1)
    dof_zip_path, db_path = sys.argv[1], sys.argv[2]

    conn = open_output_db(db_path)
    print("Importing obstacles from DOF...")
    with zipfile.ZipFile(dof_zip_path) as dof_zf:
        build_obstacles(conn, dof_zf)
    conn.commit()
    conn.close()


if __name__ == "__main__":
    main()
