#!/usr/bin/env python3
"""Full NASR DB build: runs every per-source ingester plus the search index.

Usage:
    python tools/build_all.py <csv.zip> <shapefile.zip> <aixm.zip> \\
                              <dof.zip> <adiz.geojson> <output.db>
"""

import sys
import zipfile

from build_adiz import build_adiz
from build_aixm import build_sua
from build_common import open_output_db
from build_dof import build_obstacles
from build_nasr import build_all_nasr
from build_search import build_search_index
from build_shp import build_cls_arsp


def main():
    if len(sys.argv) != 7:
        print(f"Usage: {sys.argv[0]} <csv.zip> <shapefile.zip> <aixm.zip> "
              f"<dof.zip> <adiz.geojson> <output.db>")
        sys.exit(1)
    csv_zip, shp_zip, aixm_zip, dof_zip, adiz_path, db_path = sys.argv[1:]

    conn = open_output_db(db_path, fresh=True)
    with zipfile.ZipFile(csv_zip) as zf:
        build_all_nasr(conn, zf)
    with zipfile.ZipFile(shp_zip) as zf:
        build_cls_arsp(conn, zf)
    with zipfile.ZipFile(aixm_zip) as zf:
        build_sua(conn, zf)
    with zipfile.ZipFile(dof_zip) as zf:
        build_obstacles(conn, zf)
    build_adiz(conn, adiz_path)
    build_search_index(conn)
    conn.commit()
    conn.close()


if __name__ == "__main__":
    main()
