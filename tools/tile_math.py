"""Shared Web Mercator tile-grid helpers."""

import math

EARTH_RADIUS_M = 6378137.0
WORLD_CIRCUMFERENCE_M = 2 * math.pi * EARTH_RADIUS_M


def tile_bounds_3857(z, x, y):
    """Return (xmin, ymin, xmax, ymax) in EPSG:3857 for tile z/x/y."""
    n = 2 ** z
    xmin = (x / n - 0.5) * WORLD_CIRCUMFERENCE_M
    xmax = ((x + 1) / n - 0.5) * WORLD_CIRCUMFERENCE_M
    ymax = (0.5 - y / n) * WORLD_CIRCUMFERENCE_M
    ymin = (0.5 - (y + 1) / n) * WORLD_CIRCUMFERENCE_M
    return (xmin, ymin, xmax, ymax)


def parse_zoom_range(s):
    """Parse a zoom range like '0-8' or '5' into (min_zoom, max_zoom)."""
    if "-" in s:
        lo, hi = s.split("-", 1)
        return int(lo), int(hi)
    z = int(s)
    return z, z
