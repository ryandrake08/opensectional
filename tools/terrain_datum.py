"""Convert DEM elevations to EGM2008 using cached PROJ grids."""

import os

import pyproj.datadir
from pyproj import Transformer

from download_terrain import download_tile
from terrain_datasets import SourceTile

GRID_CACHE_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "proj_grids")
_PROJ_CDN = "https://cdn.proj.org"

TARGET_DATUM = "EGM2008"
_TARGET_GRID = "us_nga_egm08_25.tif"

# Source geoid grid by native datum.
_NATIVE_DATUM_GRIDS = {
    "EGM96": "us_nga_egm96_15.tif",
}

# Independently verified conversion checks.
_CANARY = {
    "EGM96": {"lon": -122.4, "lat": 37.6, "expected_delta_m": -0.0652, "tolerance_m": 0.001},
}

# Reject implausible geoid-model shifts.
_MAX_PLAUSIBLE_DELTA_M = 20.0

_transformer_cache = {}


def _verify_transformer(native_datum, transformer):
    canary = _CANARY[native_datum]
    _, _, delta = transformer.transform(canary["lon"], canary["lat"], 0.0)
    if abs(delta) > _MAX_PLAUSIBLE_DELTA_M or abs(delta - canary["expected_delta_m"]) > canary["tolerance_m"]:
        raise RuntimeError(
            f"{native_datum}->{TARGET_DATUM} conversion failed its correctness check: "
            f"expected a {canary['expected_delta_m']:+.4f} m shift at "
            f"({canary['lon']}, {canary['lat']}) but got {delta:+.4f} m. "
            f"The grid file(s) in {GRID_CACHE_DIR} are likely missing, corrupt, or "
            "the wrong file -- delete them and re-run to re-fetch, or investigate "
            "before trusting any tile built with this datum."
        )


def _ensure_grid(filename):
    """Download a missing PROJ grid into the local cache."""
    dest_path = os.path.join(GRID_CACHE_DIR, filename)
    if os.path.exists(dest_path):
        return dest_path
    os.makedirs(GRID_CACHE_DIR, exist_ok=True)
    tile = SourceTile(url=f"{_PROJ_CDN}/{filename}", local_name=filename)
    download_tile(tile, dest_path)
    print(f"Fetched {filename} (NGA/PROJ geoid grid, one-time)")
    return dest_path


def ensure_grids(native_datum):
    """Fetch the grids needed for a source datum."""
    if native_datum == TARGET_DATUM:
        return
    if native_datum not in _NATIVE_DATUM_GRIDS:
        known = ", ".join(sorted(_NATIVE_DATUM_GRIDS))
        raise ValueError(
            f"no EGM2008 conversion registered for vertical datum {native_datum!r} "
            f"(known: {known})"
        )
    _ensure_grid(_NATIVE_DATUM_GRIDS[native_datum])
    _ensure_grid(_TARGET_GRID)


def _transformer_for(native_datum):
    """Build and cache a datum transformer."""
    if native_datum in _transformer_cache:
        return _transformer_cache[native_datum]

    ensure_grids(native_datum)
    source_grid = _NATIVE_DATUM_GRIDS[native_datum]
    pyproj.datadir.append_data_dir(GRID_CACHE_DIR)

    pipeline = (
        "+proj=pipeline "
        f"+step +proj=vgridshift +grids={source_grid} +multiplier=1 "
        f"+step +proj=vgridshift +grids={_TARGET_GRID} +multiplier=-1"
    )
    transformer = Transformer.from_pipeline(pipeline)
    _verify_transformer(native_datum, transformer)
    _transformer_cache[native_datum] = transformer
    return transformer


def verify_datum_conversion(native_datum):
    """Validate a datum conversion before workers start."""
    if native_datum == TARGET_DATUM:
        return
    _transformer_for(native_datum)
    _transformer_cache.pop(native_datum, None)


def to_egm2008(elevation_m, lon, lat, native_datum):
    """Convert scalar or array elevations to EGM2008."""
    if native_datum == TARGET_DATUM:
        return elevation_m
    transformer = _transformer_for(native_datum)
    _, _, converted = transformer.transform(lon, lat, elevation_m)
    return converted
