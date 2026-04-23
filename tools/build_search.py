#!/usr/bin/env python3
"""Rebuild the FTS5 full-text search index over ingested NASR tables.

Must be run after the source-specific ingestion scripts have populated
their tables. Safe to re-run at any time (drops and rebuilds the index).

Usage:
    python tools/build_search.py <output.db>
"""

import sys

from build_common import open_output_db


def build_search_index(conn):
    """Populate FTS5 full-text search index over top-level features.

    Columns:
      entity_type  — short code identifying source table (APT, NAV, ...)
      entity_rowid — rowid in the source table; caller re-queries for details
      ids          — identifiers (ICAO, FAA id, designator, etc.) — rank tier 1
      name         — human-readable name                          — rank tier 2
      extra        — reserved for future fields                   — rank tier 3
    """
    conn.execute("DROP TABLE IF EXISTS search_fts")
    conn.execute("""
        CREATE VIRTUAL TABLE search_fts USING fts5(
            entity_type UNINDEXED,
            entity_rowid UNINDEXED,
            ids,
            name,
            extra,
            tokenize = 'unicode61'
        )
    """)

    # Per-entity (entity_type, source_table, sql-projection) — the projection
    # yields (rowid, ids, name, extra). Empty strings for unused columns;
    # NULLs coalesced away so FTS doesn't index the literal 'null'.
    sources = [
        ("APT",   "SELECT rowid, COALESCE(ARPT_ID,'')||' '||COALESCE(ICAO_ID,''), COALESCE(ARPT_NAME,''), '' FROM APT_BASE"),
        ("NAV",   "SELECT rowid, COALESCE(NAV_ID,''), COALESCE(NAME,''), '' FROM NAV_BASE"),
        ("FIX",   "SELECT rowid, COALESCE(FIX_ID,''), '', '' FROM FIX_BASE"),
        ("AWY",   "SELECT rowid, COALESCE(AWY_ID,''), '', '' FROM AWY_BASE"),
        ("SUA",   "SELECT rowid, COALESCE(DESIGNATOR,''), COALESCE(NAME,''), '' FROM SUA_BASE"),
        ("CLS",   "SELECT rowid, '', COALESCE(NAME,''), '' FROM CLS_ARSP_BASE"),
        ("ARTCC", "SELECT rowid, COALESCE(LOCATION_ID,''), COALESCE(LOCATION_NAME,''), '' FROM ARTCC_BASE"),
        ("FSS",   "SELECT rowid, COALESCE(FSS_ID,''), COALESCE(NAME,''), '' FROM FSS_BASE"),
        ("AWOS",  "SELECT rowid, COALESCE(ASOS_AWOS_ID,''), '', '' FROM AWOS"),
        ("COM",   "SELECT rowid, '', COALESCE(COMM_OUTLET_NAME,''), '' FROM COM"),
        ("MTR",   "SELECT rowid, COALESCE(ROUTE_ID,''), '', '' FROM MTR_BASE"),
        ("PJA",   "SELECT rowid, COALESCE(PJA_ID,''), COALESCE(NAME,''), '' FROM PJA_BASE"),
        ("ADIZ",  "SELECT rowid, '', COALESCE(NAME,''), '' FROM ADIZ_BASE"),
    ]

    total = 0
    for entity_type, projection in sources:
        rows = conn.execute(projection).fetchall()
        filtered = [(entity_type, r[0], r[1], r[2], r[3]) for r in rows
                    if (r[1] or '').strip() or (r[2] or '').strip() or (r[3] or '').strip()]
        conn.executemany(
            "INSERT INTO search_fts (entity_type, entity_rowid, ids, name, extra) VALUES (?, ?, ?, ?, ?)",
            filtered,
        )
        print(f"  search_fts {entity_type}: {len(filtered)} rows")
        total += len(filtered)
    print(f"  search_fts total: {total} rows")


def main():
    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} <output.db>")
        sys.exit(1)
    db_path = sys.argv[1]

    conn = open_output_db(db_path)
    print("Building search index...")
    build_search_index(conn)
    conn.commit()
    conn.close()


if __name__ == "__main__":
    main()
