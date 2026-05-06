"""Helpers shared across the per-source ingestion scripts.

Kept deliberately small: utilities here are imported by two or more of
build_nasr / build_shp / build_aixm / build_adiz. Source-local helpers
live next to the build_* function that uses them.
"""

import datetime
import os
import re
import sqlite3


# Sentinel for "unlimited" altitude. Any real altitude compared against
# this will register as below it. Stored as an INT in the DB.
ALT_UNLIMITED_FT = 99999


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


def simplify_ring(points, epsilon):
    """Simplify a polygon ring using Shapely's GEOS-backed RDP.

    Points are (lon, lat) tuples. Epsilon is in degrees.
    """
    if len(points) <= 3:
        return points
    from shapely.geometry import LineString
    simplified = LineString(points).simplify(epsilon, preserve_topology=False)
    return list(simplified.coords)


def _parse_date_loose(s):
    """Parse a date string into ISO YYYY-MM-DD form.

    Accepts the common formats actually seen in FAA data:
      "2026/04/16"      → "2026-04-16"   (NASR EFF_DATE)
      "2026-04-16"      → "2026-04-16"
      "02/15/26"        → "2026-02-15"   (DOF CURRENCY DATE)
      "02/15/2026"      → "2026-02-15"
    Returns None on failure rather than raising.
    """
    if s is None:
        return None
    s = s.strip()
    if not s:
        return None
    for fmt in ("%Y/%m/%d", "%Y-%m-%d", "%m/%d/%y", "%m/%d/%Y"):
        try:
            return datetime.datetime.strptime(s, fmt).date().isoformat()
        except ValueError:
            continue
    return None


def normalize_iso_date(s):
    """Normalize an FAA date column value to an ISO 8601 string.

    The osect.db convention is that every date-bearing column stores
    ISO 8601, with the precision the source actually provides preserved
    (a YYYY/MM activation date stays YYYY-MM rather than getting padded
    to a fake day). Handles every form the ingest pipeline encounters:

      "2026/04/16"   → "2026-04-16"   (NASR full date)
      "2026/04"      → "2026-04"      (NASR partial — APT_BASE.ACTIVATION_DATE)
      "2026"         → "2026"         (year only)
      "2014138"      → "2014-05-18"   (DOF Julian YYYYJJJ)
      "2026-04-16"   → "2026-04-16"   (already ISO; pass through)
      "2026-04"      → "2026-04"      (idempotent)
      "" / None      → None

    Returns None for unrecognized or invalid input — FAA data is the
    project's outer boundary, where we drop the field rather than
    aborting the whole build.
    """
    if s is None:
        return None
    s = str(s).strip()
    if not s:
        return None

    # Already ISO. Validate only the full-date form via fromisoformat.
    if re.fullmatch(r"\d{4}-\d{2}-\d{2}", s):
        try:
            datetime.date.fromisoformat(s)
            return s
        except ValueError:
            return None
    if re.fullmatch(r"\d{4}-\d{2}", s):
        if 1 <= int(s[5:7]) <= 12:
            return s
        return None
    if re.fullmatch(r"\d{4}", s):
        return s

    # NASR slash forms.
    m = re.fullmatch(r"(\d{4})/(\d{2})/(\d{2})", s)
    if m:
        try:
            return datetime.date(int(m[1]), int(m[2]), int(m[3])).isoformat()
        except ValueError:
            return None
    m = re.fullmatch(r"(\d{4})/(\d{2})", s)
    if m:
        if 1 <= int(m[2]) <= 12:
            return f"{int(m[1]):04d}-{int(m[2]):02d}"
        return None

    # DOF Julian YYYYJJJ (year + day-of-year).
    m = re.fullmatch(r"(\d{4})(\d{3})", s)
    if m:
        y, doy = int(m[1]), int(m[2])
        try:
            d = datetime.date(y, 1, 1) + datetime.timedelta(days=doy - 1)
            if d.year != y:  # day-of-year overflow into next year
                return None
            return d.isoformat()
        except (ValueError, OverflowError):
            return None

    return None


def normalize_date_column(conn, table, column):
    """Rewrite a TEXT date column from raw FAA forms to ISO 8601 in-place.

    Apply once after `import_csv` for every column that carries a date.
    Values that fail to parse become NULL — see `normalize_iso_date`.
    """
    rows = conn.execute(
        f"SELECT rowid, \"{column}\" FROM \"{table}\" "
        f"WHERE \"{column}\" IS NOT NULL AND \"{column}\" != ''"
    ).fetchall()
    updates = []
    for rowid, raw in rows:
        iso = normalize_iso_date(raw)
        if iso != raw:
            updates.append((iso, rowid))
    if updates:
        conn.executemany(
            f"UPDATE \"{table}\" SET \"{column}\" = ? WHERE rowid = ?",
            updates)


def read_meta(conn, name):
    """Return (effective, expires) ISO strings for a META row, or
    (None, None) if the row is missing or the table doesn't exist yet.

    Used by AIXM/SHP ingesters: those files are bundled with the
    NASR cycle but carry stale internal timestamps that don't track
    the cycle, so they inherit from the nasr row instead."""
    try:
        cur = conn.execute(
            "SELECT effective, expires FROM META WHERE name = ?",
            (name,))
    except sqlite3.OperationalError:
        return (None, None)
    row = cur.fetchone()
    if row is None:
        return (None, None)
    return (row[0], row[1])


def write_meta(conn, name, *, kind="static",
               effective=None, expires=None, info=""):
    """Upsert a META row for one data source.

    Each ingester writes one row. Kept narrow on purpose: the registry
    table is the single point of truth that the C++ side reads to decide
    whether a source is fresh or expired, so its schema needs to be
    stable across the ingesters.

      name: short identifier ("nasr", "shp", "aixm", "dof", "adiz",
            "tfr"). Primary key — re-running an ingester replaces.
      kind: "static" (cycle-driven, refreshed by re-running this script)
            or "ephemeral" (in-app refresh, not yet implemented; reserved).
      effective / expires: ISO 8601 dates the *source* declares — e.g.
            NASR's EFF_DATE column or DOF's CURRENCY DATE header. Pass
            None when the source carries no explicit date; the C++ side
            then reports the row as `unknown` rather than synthesizing
            a fake expiration.
      info: short human description for the UI ("Cycle 16 Apr 2026").
    """
    conn.execute("""
        CREATE TABLE IF NOT EXISTS META (
            name TEXT PRIMARY KEY,
            kind TEXT NOT NULL,
            last_updated TEXT NOT NULL,
            effective TEXT,
            expires TEXT,
            info TEXT
        )
    """)
    last_updated = (datetime.datetime.now(datetime.timezone.utc)
                    .replace(microsecond=0).isoformat())
    conn.execute(
        "INSERT OR REPLACE INTO META "
        "(name, kind, last_updated, effective, expires, info) "
        "VALUES (?, ?, ?, ?, ?, ?)",
        (name, kind, last_updated, effective, expires, info or ""))


def open_output_db(path, fresh=False):
    """Open the output SQLite DB with the ingestion PRAGMAs.

    If `fresh` is True, any existing file at `path` is removed first — only
    the orchestrator (build_all) passes this. Per-source scripts always open
    with fresh=False so they preserve other sources' tables.
    """
    if fresh and os.path.exists(path):
        os.remove(path)
    conn = sqlite3.connect(path)
    conn.execute("PRAGMA journal_mode=OFF")
    conn.execute("PRAGMA synchronous=OFF")
    conn.execute("PRAGMA cache_size=-200000")  # 200 MB
    return conn
