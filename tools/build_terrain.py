#!/usr/bin/env python3
"""Build a terrain tile pyramid from downloaded DEM rasters.

Top-level tiles are reprojected to Web Mercator. Lower levels max-pool
child interiors, then copy skirts from same-zoom neighbours.

Usage:
    python3 tools/build_terrain.py --dataset gmted2010-30 --zoom 0-6 \\
        terrain_source/gmted2010-30 terrain
"""

import argparse
import contextlib
import math
import multiprocessing
import os
import time

import numpy as np
import rasterio
import rasterio.features
import rasterio.transform
from rasterio.merge import merge as rasterio_merge
from rasterio.warp import Resampling, reproject

import terrain_common
import terrain_datum
import terrain_manifest
from terrain_datasets import get_adapter
from tile_math import (
    EARTH_RADIUS_M,
    WORLD_CIRCUMFERENCE_M,
    parse_zoom_range,
    tile_bounds_3857,
)

MERCATOR_MAX_LAT = 85.0511287798066  # where Web Mercator's y projection diverges

# Source selection margin for tile skirts.
_MARGIN_DEG = 0.5


def expanded_tile_bounds_3857(z, x, y):
    """A tile's Mercator bounds expanded by the skirt, i.e. the true
    260x260-pixel footprint rather than the nominal 256x256 tile."""
    xmin, ymin, xmax, ymax = tile_bounds_3857(z, x, y)
    px = (xmax - xmin) / terrain_common.TILE_PIXELS
    py = (ymax - ymin) / terrain_common.TILE_PIXELS
    skirt_x = px * terrain_common.SKIRT_PIXELS
    skirt_y = py * terrain_common.SKIRT_PIXELS
    return (xmin - skirt_x, ymin - skirt_y, xmax + skirt_x, ymax + skirt_y)


def _mx_to_lon(mx):
    return math.degrees(mx / EARTH_RADIUS_M)


def _my_to_lat(my):
    return math.degrees(2 * math.atan(math.exp(my / EARTH_RADIUS_M)) - math.pi / 2)


def mercator_bounds_to_lonlat(xmin, ymin, xmax, ymax):
    """Inverse-project a Mercator bbox to (lon_min, lat_min, lon_max, lat_max).
    Both axes are monotonic, so the corners give the extremes directly."""
    return (_mx_to_lon(xmin), _my_to_lat(ymin), _mx_to_lon(xmax), _my_to_lat(ymax))


def lonlat_to_tile_xy(lon, lat, zoom):
    """The (x, y) tile index at `zoom` containing (lon, lat)."""
    n = 2 ** zoom
    lat = max(-MERCATOR_MAX_LAT, min(MERCATOR_MAX_LAT, lat))
    x = int((lon + 180.0) / 360.0 * n)
    lat_rad = math.radians(lat)
    y = int((1.0 - math.log(math.tan(lat_rad) + 1.0 / math.cos(lat_rad)) / math.pi) / 2.0 * n)
    return max(0, min(n - 1, x)), max(0, min(n - 1, y))


def tiles_covering_bounds(lon_min, lat_min, lon_max, lat_max, zoom):
    """z/x/y tiles at `zoom` whose Mercator bounds overlap a bbox.

    `lon_min` and `lon_max` here never wrap the antimeridian -- this is
    called once per *source file*, and every supported dataset's own
    tiling scheme keeps individual file bounds within [-180, 180]
    (see build_source_index).
    """
    lat_min = max(lat_min, -MERCATOR_MAX_LAT)
    lat_max = min(lat_max, MERCATOR_MAX_LAT)
    if lat_min >= lat_max:
        return set()
    x_min, y_min = lonlat_to_tile_xy(lon_min, lat_max, zoom)  # NW corner
    x_max, y_max = lonlat_to_tile_xy(lon_max, lat_min, zoom)  # SE corner
    return {(zoom, x, y) for y in range(y_min, y_max + 1) for x in range(x_min, x_max + 1)}


def build_source_index(source_dir, adapter, *, water=False):
    """[(path, (lon_min, lat_min, lon_max, lat_max)), ...] for every
    source raster `adapter` resolves in source_dir, read once from
    each dataset's header. `water=True` indexes the water-mask sources
    instead of the elevation sources."""
    paths = adapter.water_source_paths(source_dir) if water else adapter.source_paths(source_dir)
    index = []
    for path in paths:
        with rasterio.open(path) as dataset:
            b = dataset.bounds
            index.append((path, (b.left, b.bottom, b.right, b.top)))
    return index


def _overlaps(a, b):
    a_lon_min, a_lat_min, a_lon_max, a_lat_max = a
    b_lon_min, b_lat_min, b_lon_max, b_lat_max = b
    return a_lon_min < b_lon_max and a_lon_max > b_lon_min and a_lat_min < b_lat_max and a_lat_max > b_lat_min


def relevant_sources(index, bbox):
    return [path for path, bounds in index if _overlaps(bounds, bbox)]


def build_tile(z, x, y, adapter, index, output_dir, force=False):
    """Build one output tile. Returns "built", "skipped" (already
    present), or "empty" (no real source coverage)."""
    tile_path = _tile_path(z, x, y, output_dir)
    if not force and os.path.exists(tile_path):
        return "skipped"

    merc_bounds = expanded_tile_bounds_3857(z, x, y)
    lon_min, lat_min, lon_max, lat_max = mercator_bounds_to_lonlat(*merc_bounds)
    window = (lon_min - _MARGIN_DEG, lat_min - _MARGIN_DEG, lon_max + _MARGIN_DEG, lat_max + _MARGIN_DEG)

    sources = relevant_sources(index, window)
    if not sources:
        return "empty"

    merged, merged_transform = rasterio_merge(
        sources, bounds=window, nodata=adapter.native_nodata, method="max"
    )
    merged = merged[0].astype(np.float64)  # single band
    if adapter.native_nodata is not None:
        merged[merged == adapter.native_nodata] = np.nan
    if np.all(np.isnan(merged)):
        return "empty"

    rows, cols = np.indices(merged.shape)
    lons, lats = merged_transform * (cols + 0.5, rows + 0.5)
    merged = terrain_datum.to_egm2008(merged, lons, lats, adapter.native_vertical_datum)

    full = terrain_common.FULL_TILE_PIXELS
    dst_transform = rasterio.transform.from_bounds(*merc_bounds, full, full)
    destination = np.full((full, full), np.nan, dtype=np.float64)
    reproject(
        source=merged,
        destination=destination,
        src_transform=merged_transform,
        src_crs="EPSG:4326",
        dst_transform=dst_transform,
        dst_crs="EPSG:3857",
        src_nodata=np.nan,
        dst_nodata=np.nan,
        resampling=Resampling.max,
    )
    if np.all(np.isnan(destination)):
        # The source had real data (checked above) -- an all-no-data
        # result here can only mean the reprojection itself failed to
        # transfer it, not that the tile is legitimately uncovered.
        # Treating this as "empty" would silently drop real terrain.
        raise RuntimeError(
            f"tile {z}/{x}/{y}: source had {np.count_nonzero(~np.isnan(merged))} valid "
            "pixel(s) but the Mercator reprojection produced none -- this is a "
            "reprojection failure, not a legitimately empty tile"
        )

    _write_tile(tile_path, destination, adapter)
    return "built"


# --- Pyramid reduction -----------------------------------------------

def _tile_path(z, x, y, output_dir):
    return os.path.join(output_dir, str(z), str(x), f"{y}.png")


def _load_interior(z, x, y, output_dir):
    """The 256x256 interior of a built tile, or None if it doesn't exist."""
    path = _tile_path(z, x, y, output_dir)
    if not os.path.exists(path):
        return None
    elevation = terrain_common.decode_elevation_array(terrain_common.load_tile_png(path))
    return terrain_common.extract_interior(elevation)


def candidate_parents(child_zoom, output_dir):
    """The set of (x, y) at child_zoom - 1 with at least one built
    child tile at child_zoom, found by scanning the tiles actually on
    disk -- the same source of truth a resumed run would see."""
    parents = set()
    child_dir = os.path.join(output_dir, str(child_zoom))
    if not os.path.isdir(child_dir):
        return parents
    for x_name in os.listdir(child_dir):
        x_dir = os.path.join(child_dir, x_name)
        if not x_name.isdigit() or not os.path.isdir(x_dir):
            continue
        x = int(x_name)
        for fname in os.listdir(x_dir):
            y_name, extension = os.path.splitext(fname)
            if extension == ".png" and y_name.isdigit():
                y = int(y_name)
                parents.add((x // 2, y // 2))
    return parents


def pool_children(parent_z, parent_x, parent_y, output_dir):
    """Max-pool child interiors into one parent interior."""
    child_z = parent_z + 1
    tp = terrain_common.TILE_PIXELS
    merged = np.full((tp * 2, tp * 2), np.nan)
    any_data = False
    for dy in (0, 1):
        for dx in (0, 1):
            interior = _load_interior(child_z, parent_x * 2 + dx, parent_y * 2 + dy, output_dir)
            if interior is None:
                continue
            merged[dy * tp:(dy + 1) * tp, dx * tp:(dx + 1) * tp] = interior
            if np.any(~np.isnan(interior)):
                any_data = True
    if not any_data:
        return None

    # Preserve valid samples beside no-data.
    filled = np.where(np.isnan(merged), -np.inf, merged)
    pooled = filled.reshape(tp, 2, tp, 2).max(axis=(1, 3))
    pooled[np.isinf(pooled)] = np.nan
    return pooled


def pool_tile(parent_z, parent_x, parent_y, adapter, output_dir, force=False):
    """Build one pyramid tile with an empty skirt."""
    tile_path = _tile_path(parent_z, parent_x, parent_y, output_dir)
    if not force and os.path.exists(tile_path):
        return "skipped"

    interior = pool_children(parent_z, parent_x, parent_y, output_dir)
    if interior is None:
        return "empty"

    full_array = np.full((terrain_common.FULL_TILE_PIXELS, terrain_common.FULL_TILE_PIXELS), np.nan)
    rows, cols = terrain_common.interior_slice()
    full_array[rows, cols] = interior
    _write_tile(tile_path, full_array, adapter)
    return "built"


# Skirt slices and matching neighbor interior slices.
_S = terrain_common.SKIRT_PIXELS
_SKIRT_SIDES = {
    "N": ((slice(None, _S), slice(_S, -_S)), lambda i: i[-_S:, :]),
    "S": ((slice(-_S, None), slice(_S, -_S)), lambda i: i[:_S, :]),
    "W": ((slice(_S, -_S), slice(None, _S)), lambda i: i[:, -_S:]),
    "E": ((slice(_S, -_S), slice(-_S, None)), lambda i: i[:, :_S]),
    "NW": ((slice(None, _S), slice(None, _S)), lambda i: i[-_S:, -_S:]),
    "NE": ((slice(None, _S), slice(-_S, None)), lambda i: i[-_S:, :_S]),
    "SW": ((slice(-_S, None), slice(None, _S)), lambda i: i[:_S, -_S:]),
    "SE": ((slice(-_S, None), slice(-_S, None)), lambda i: i[:_S, :_S]),
}
_NEIGHBOR_OFFSET = {
    "N": (0, -1), "S": (0, 1), "E": (1, 0), "W": (-1, 0),
    "NE": (1, -1), "NW": (-1, -1), "SE": (1, 1), "SW": (-1, 1),
}


def _neighbor_xy(x, y, side, n):
    """(x, y) of the tile in `side` direction at the same zoom (n =
    2**zoom), or None if it would fall off the top/bottom of the world.
    Longitude wraps (x % n); latitude does not."""
    dx, dy = _NEIGHBOR_OFFSET[side]
    ny = y + dy
    if ny < 0 or ny >= n:
        return None
    return (x + dx) % n, ny


def stitch_skirt(z, x, y, adapter, output_dir):
    """Fill a tile's skirt from same-zoom neighbors."""
    tile_path = _tile_path(z, x, y, output_dir)
    elevation = terrain_common.decode_elevation_array(terrain_common.load_tile_png(tile_path))
    n = 2 ** z

    for side, ((row_slice, col_slice), extract) in _SKIRT_SIDES.items():
        neighbor = _neighbor_xy(x, y, side, n)
        if neighbor is None:
            continue
        neighbor_interior = _load_interior(z, *neighbor, output_dir)
        if neighbor_interior is None:
            continue
        elevation[row_slice, col_slice] = extract(neighbor_interior)

    _write_tile(tile_path, elevation, adapter)


def _write_tile(tile_path, elevation, adapter):
    """Encode and atomically write one PNG tile."""
    rgb = terrain_common.encode_elevation_array(elevation, adapter.vertical_precision_m)
    os.makedirs(os.path.dirname(tile_path), exist_ok=True)
    tmp_path = tile_path[: -len(".png")] + ".tmp.png"
    terrain_common.save_tile_png(tmp_path, rgb)
    os.replace(tmp_path, tile_path)


def all_tiles_at(zoom, output_dir):
    """[(zoom, x, y), ...] for every tile file that exists at `zoom`."""
    tiles = []
    level_dir = os.path.join(output_dir, str(zoom))
    if not os.path.isdir(level_dir):
        return tiles
    x_names = sorted(
        (name for name in os.listdir(level_dir)
         if name.isdigit() and os.path.isdir(os.path.join(level_dir, name))),
        key=int,
    )
    for x_name in x_names:
        x = int(x_name)
        for fname in sorted(os.listdir(os.path.join(level_dir, x_name))):
            y_name, extension = os.path.splitext(fname)
            if extension == ".png" and y_name.isdigit():
                tiles.append((zoom, x, int(y_name)))
    return tiles


def scan_built_coverage(output_dir):
    """Return the zoom range and bbox represented by tiles on disk."""
    zoom_levels = sorted(
        int(name) for name in os.listdir(output_dir)
        if name.isdigit() and os.path.isdir(os.path.join(output_dir, name)) and all_tiles_at(int(name), output_dir)
    )
    if not zoom_levels:
        raise ValueError(f"no built tiles found in {output_dir}; nothing to write a manifest for")

    min_zoom, max_zoom = zoom_levels[0], zoom_levels[-1]
    gaps = [z for z in range(min_zoom, max_zoom + 1) if z not in zoom_levels]
    if gaps:
        print(f"WARNING: zoom level(s) {gaps} have no tiles -- the pyramid has a gap")

    # The finest zoom defines coverage.
    tiles = all_tiles_at(max_zoom, output_dir)
    xs = [x for _, x, _ in tiles]
    ys = [y for _, _, y in tiles]
    lon_min, _, _, lat_max = mercator_bounds_to_lonlat(*tile_bounds_3857(max_zoom, min(xs), min(ys)))
    _, lat_min, lon_max, _ = mercator_bounds_to_lonlat(*tile_bounds_3857(max_zoom, max(xs), max(ys)))

    # Keep round-tripped boundaries inside their tiles.
    eps = 1e-7
    return min_zoom, max_zoom, (lon_min + eps, lat_min + eps, lon_max - eps, lat_max - eps)


# --- Worker plumbing --------------------------------------------------

_worker_adapter = None
_worker_index = None


def _worker_init(dataset_name, index):
    global _worker_adapter, _worker_index
    _worker_adapter = get_adapter(dataset_name)
    _worker_index = index


def _build_and_report(args):
    z, x, y, output_dir, force = args
    return build_tile(z, x, y, _worker_adapter, _worker_index, output_dir, force=force)


# Pyramid workers need only the adapter.
_pyramid_worker_adapter = None


def _pyramid_worker_init(dataset_name):
    global _pyramid_worker_adapter
    _pyramid_worker_adapter = get_adapter(dataset_name)


def _pool_and_report(args):
    z, x, y, output_dir, force = args
    return pool_tile(z, x, y, _pyramid_worker_adapter, output_dir, force=force)


def _stitch_and_report(args):
    z, x, y, output_dir = args
    stitch_skirt(z, x, y, _pyramid_worker_adapter, output_dir)
    return None


def _run_pooled(tile_args, worker_fn, worker_init, worker_init_args, num_workers):
    """Run work directly or in a fork-based pool."""
    if num_workers == 1 or not tile_args:
        worker_init(*worker_init_args)
        return [worker_fn(ta) for ta in tile_args]
    ctx = multiprocessing.get_context("fork")
    with ctx.Pool(num_workers, initializer=worker_init, initargs=worker_init_args) as pool:
        return list(pool.imap_unordered(worker_fn, tile_args, chunksize=8))


@contextlib.contextmanager
def pyramid_pool(dataset_name, num_workers):
    """Share one worker pool across pyramid levels."""
    if num_workers == 1:
        _pyramid_worker_init(dataset_name)
        yield None
        return
    ctx = multiprocessing.get_context("fork")
    with ctx.Pool(num_workers, initializer=_pyramid_worker_init, initargs=(dataset_name,)) as pool:
        yield pool


def _run_on_pool(tile_args, worker_fn, pool):
    """Run work on a shared pool or directly."""
    if pool is None or not tile_args:
        return [worker_fn(ta) for ta in tile_args]
    return list(pool.imap_unordered(worker_fn, tile_args, chunksize=8))


def build_pyramid_level(parent_zoom, output_dir, pool, force=False):
    """Pool child tiles into one pyramid level."""
    parents = sorted(candidate_parents(parent_zoom + 1, output_dir))
    tile_args = [(parent_zoom, x, y, output_dir, force) for x, y in parents]
    results = _run_on_pool(tile_args, _pool_and_report, pool)
    return (results.count("built"), results.count("skipped"), results.count("empty"))


def stitch_level_skirts(zoom, output_dir, pool):
    """Stitch every skirt at one zoom level."""
    tiles = all_tiles_at(zoom, output_dir)
    tile_args = [(z, x, y, output_dir) for z, x, y in tiles]
    _run_on_pool(tile_args, _stitch_and_report, pool)
    return len(tiles)


# --- Water-mask pass -------------------------------------------------
#
# A parallel `water/z/x/y.png` tree beside the height tiles, same zoom
# range and tile + skirt geometry: the class (terrain_common.WATER_*)
# rides in the R channel. All functions take the water root
# (output_dir/water) directly, so candidate_parents / all_tiles_at work
# on it unchanged. A per-adapter provider turns the dataset's water
# source into a class array for one tile: the Copernicus WBM raster, or
# rasterised GSHHG shoreline polygons for GMTED.

#: GSHHG level -> class painted for it. Painter's order: land over
#: ocean, lake over land, island-in-lake over lake, pond over island.
#: L5/L6 (Antarctic ice front / grounding line) are skipped -- that far
#: south is ice, not water, and outside GMTED's coverage.
_GSHHG_LEVELS = ((1, 0), (2, 2), (3, 0), (4, 2))  # (level, WATER_* class)


def _water_tile_path(z, x, y, water_dir):
    return os.path.join(water_dir, str(z), str(x), f"{y}.png")


def _write_water_tile(tile_path, classes):
    rgb = terrain_common.encode_water_array(classes)
    os.makedirs(os.path.dirname(tile_path), exist_ok=True)
    tmp_path = tile_path[: -len(".png")] + ".tmp.png"
    terrain_common.save_tile_png(tmp_path, rgb)
    os.replace(tmp_path, tile_path)


def _load_water_interior(z, x, y, water_dir):
    """The 256x256 class interior of a built water tile, or None."""
    path = _water_tile_path(z, x, y, water_dir)
    if not os.path.exists(path):
        return None
    classes = terrain_common.decode_water_array(terrain_common.load_tile_png(path))
    return terrain_common.extract_interior(classes)


class _WbmWaterProvider:
    """Reprojects the Copernicus Water Body Mask rasters into a tile's
    class grid. Returns None where no WBM source covers the tile."""

    def __init__(self, index):
        self._index = index

    def __call__(self, z, x, y):
        merc_bounds = expanded_tile_bounds_3857(z, x, y)
        lon_min, lat_min, lon_max, lat_max = mercator_bounds_to_lonlat(*merc_bounds)
        window = (lon_min - _MARGIN_DEG, lat_min - _MARGIN_DEG,
                  lon_max + _MARGIN_DEG, lat_max + _MARGIN_DEG)
        sources = relevant_sources(self._index, window)
        if not sources:
            return None
        # Class 0 (no water) acts as merge/reproject nodata: land never
        # overwrites anything and the margin fills as land, so only water
        # classes are painted into `destination`.
        merged, merged_transform = rasterio_merge(sources, bounds=window, nodata=0)
        merged = merged[0].astype(np.uint8)
        full = terrain_common.FULL_TILE_PIXELS
        dst_transform = rasterio.transform.from_bounds(*merc_bounds, full, full)
        destination = np.zeros((full, full), dtype=np.uint8)
        reproject(
            source=merged, destination=destination,
            src_transform=merged_transform, src_crs="EPSG:4326",
            dst_transform=dst_transform, dst_crs="EPSG:3857",
            src_nodata=0, dst_nodata=0,
            resampling=Resampling.nearest,
        )
        return destination


class _GshhgWaterProvider:
    """Rasterises GSHHG shoreline polygons into a tile's class grid.
    Always returns a full array (ocean where nothing else applies)."""

    def __init__(self, zip_path, resolution):
        import zipfile

        import fiona
        from pyproj import Transformer
        from shapely import STRtree
        from shapely.geometry import box, shape
        from shapely.ops import transform as shapely_transform

        self._box = box
        to_merc = Transformer.from_crs("EPSG:4326", "EPSG:3857", always_xy=True).transform
        clip = box(-180.0, -MERCATOR_MAX_LAT, 180.0, MERCATOR_MAX_LAT)
        present = set(zipfile.ZipFile(zip_path).namelist())

        self._layers = []  # [(paint_class, [merc geoms], STRtree), ...]
        for level, paint_class in _GSHHG_LEVELS:
            entry = f"GSHHS_shp/{resolution}/GSHHS_{resolution}_L{level}.shp"
            if entry not in present:
                continue  # L3/L4 are sparse and may be absent
            geoms = []
            with fiona.open(f"zip://{zip_path}!/{entry}") as src:
                for feat in src:
                    g = shape(feat["geometry"])
                    if not g.intersects(clip):
                        continue
                    if not g.within(clip):
                        g = g.intersection(clip)
                    if not g.is_empty:
                        geoms.append(shapely_transform(to_merc, g))
            self._layers.append((paint_class, geoms, STRtree(geoms) if geoms else None))

    def __call__(self, z, x, y):
        merc = expanded_tile_bounds_3857(z, x, y)
        full = terrain_common.FULL_TILE_PIXELS
        transform = rasterio.transform.from_bounds(*merc, full, full)
        tile_box = self._box(*merc)
        classes = np.full((full, full), terrain_common.WATER_OCEAN, dtype=np.uint8)
        for paint_class, geoms, tree in self._layers:
            if tree is None:
                continue
            shapes = [(geoms[i], 1) for i in tree.query(tile_box) if geoms[i].intersects(tile_box)]
            if not shapes:
                continue
            # Land polygons (L1, L3) grab every pixel they touch so a
            # coastline hugs the land side; water bodies (L2, L4) only
            # take pixels whose centre is inside, so they don't grow.
            hit = rasterio.features.rasterize(shapes, out_shape=(full, full), transform=transform,
                                              fill=0, dtype=np.uint8,
                                              all_touched=(paint_class == terrain_common.WATER_NONE))
            classes[hit == 1] = paint_class
        return classes


def _make_water_provider(source_dir, adapter):
    """The water provider for `adapter`, or None if it has no usable
    water source in `source_dir`."""
    src = getattr(adapter, "water_mask_source", None)
    if src == "copernicus-wbm":
        index = build_source_index(source_dir, adapter, water=True)
        return _WbmWaterProvider(index) if index else None
    if src == "gshhg":
        zip_path = adapter.gshhg_zip_path(source_dir)
        return _GshhgWaterProvider(zip_path, adapter.gshhg_resolution) if zip_path else None
    return None


def build_water_tile(z, x, y, provider, water_dir, force=False):
    """Build one water tile. Returns "built", "skipped" (already
    present), or "empty" (no source coverage, or all-land)."""
    tile_path = _water_tile_path(z, x, y, water_dir)
    if not force and os.path.exists(tile_path):
        return "skipped"
    classes = provider(z, x, y)
    if classes is None or not classes.any():
        return "empty"
    _write_water_tile(tile_path, classes)
    return "built"


def pool_water_children(parent_z, parent_x, parent_y, water_dir):
    """Reduce 4 child water interiors one level. A parent texel is water
    only when a strict majority (3 of 4) of its children are, with ties
    (2 of 4) resolving to land -- not the height pyramid's max-pool. An
    isolated child water texel drops out, a solid water body keeps its
    size, and coastlines pull landward one texel per level, countering
    the outward creep the old max-pool produced. Class is the max of the
    water children present."""
    child_z = parent_z + 1
    tp = terrain_common.TILE_PIXELS
    merged = np.zeros((tp * 2, tp * 2), dtype=np.uint8)
    any_child = False
    for dy in (0, 1):
        for dx in (0, 1):
            interior = _load_water_interior(child_z, parent_x * 2 + dx, parent_y * 2 + dy, water_dir)
            if interior is None:
                continue
            merged[dy * tp:(dy + 1) * tp, dx * tp:(dx + 1) * tp] = interior
            any_child = True
    if not any_child:
        return None
    blocks = merged.reshape(tp, 2, tp, 2)
    water_count = (blocks > 0).sum(axis=(1, 3))
    pooled = np.where(water_count >= 3, blocks.max(axis=(1, 3)), 0).astype(np.uint8)
    if not pooled.any():
        return None
    return pooled


def pool_water_tile(parent_z, parent_x, parent_y, water_dir, force=False):
    tile_path = _water_tile_path(parent_z, parent_x, parent_y, water_dir)
    if not force and os.path.exists(tile_path):
        return "skipped"
    interior = pool_water_children(parent_z, parent_x, parent_y, water_dir)
    if interior is None:
        return "empty"
    full = np.zeros((terrain_common.FULL_TILE_PIXELS, terrain_common.FULL_TILE_PIXELS), dtype=np.uint8)
    rows, cols = terrain_common.interior_slice()
    full[rows, cols] = interior
    _write_water_tile(tile_path, full)
    return "built"


def stitch_water_skirt(z, x, y, water_dir):
    """Fill a water tile's skirt from same-zoom neighbours."""
    tile_path = _water_tile_path(z, x, y, water_dir)
    classes = terrain_common.decode_water_array(terrain_common.load_tile_png(tile_path))
    n = 2 ** z
    for _side, ((row_slice, col_slice), extract) in _SKIRT_SIDES.items():
        neighbor = _neighbor_xy(x, y, _side, n)
        if neighbor is None:
            continue
        neighbor_interior = _load_water_interior(z, *neighbor, water_dir)
        if neighbor_interior is None:
            continue
        classes[row_slice, col_slice] = extract(neighbor_interior)
    _write_water_tile(tile_path, classes)


# Water workers inherit the provider (and the GSHHG provider's ~150 MB
# of geometry) from the parent via fork, so no initializer is needed.
_water_provider = None


def _water_build_worker(args):
    z, x, y, water_dir, force = args
    return build_water_tile(z, x, y, _water_provider, water_dir, force=force)


def _water_pool_worker(args):
    z, x, y, water_dir, force = args
    return pool_water_tile(z, x, y, water_dir, force=force)


def _water_stitch_worker(args):
    z, x, y, water_dir = args
    stitch_water_skirt(z, x, y, water_dir)


@contextlib.contextmanager
def water_pool(num_workers):
    """One fork-based worker pool for the whole water pass -- the top
    zoom build, its skirts, and every pyramid level. Workers inherit the
    provider (and the GSHHG provider's ~150 MB of geometry) from the
    parent via fork, so no initializer is needed."""
    if num_workers == 1:
        yield None
        return
    ctx = multiprocessing.get_context("fork")
    with ctx.Pool(num_workers) as pool:
        yield pool


def build_water_pass(source_dir, adapter, output_dir, min_zoom, max_zoom,
                     height_tiles, num_workers, force=False):
    """Build the water/ sidecar tree over the same zoom range as the
    height tiles. Returns (min_zoom, max_zoom, bbox), or None if the
    adapter has no usable water source / nothing was built."""
    global _water_provider
    _water_provider = _make_water_provider(source_dir, adapter)
    if _water_provider is None:
        return None

    water_dir = os.path.join(output_dir, "water")
    tiles = sorted({(x, y) for _, x, y in height_tiles})
    # The GSHHG provider forks ~150 MB of geometry per worker (copy on
    # write); cap the water pass below the height build's worker count.
    workers = min(num_workers, 8)
    print(f"water: {len(tiles)} candidate tile(s) at z{max_zoom}")

    with water_pool(workers) as pool:
        start = time.time()
        results = _run_on_pool([(max_zoom, x, y, water_dir, force) for x, y in tiles],
                               _water_build_worker, pool)
        level = all_tiles_at(max_zoom, water_dir)
        _run_on_pool([(z, x, y, water_dir) for z, x, y in level], _water_stitch_worker, pool)
        print(f"water z{max_zoom}: {results.count('built')} built, {results.count('skipped')} skipped, "
              f"{results.count('empty')} empty, of {len(tiles)}, in {time.time() - start:.1f}s")

        # Majority-pool the pyramid so water stays proportional at reduced
        # zoom (the height pyramid's max-pool would grow it at every step).
        for parent_zoom in range(max_zoom - 1, min_zoom - 1, -1):
            start = time.time()
            parents = sorted(candidate_parents(parent_zoom + 1, water_dir))
            pooled = _run_on_pool([(parent_zoom, x, y, water_dir, force) for x, y in parents],
                                  _water_pool_worker, pool)
            level = all_tiles_at(parent_zoom, water_dir)
            _run_on_pool([(z, x, y, water_dir) for z, x, y in level], _water_stitch_worker, pool)
            print(f"water z{parent_zoom} (pooled from z{parent_zoom + 1}): {pooled.count('built')} built, "
                  f"{pooled.count('skipped')} skipped, {len(level)} skirt(s) stitched, in {time.time() - start:.1f}s")

    try:
        return min_zoom, max_zoom, scan_built_coverage(water_dir)[2]
    except ValueError:
        return None  # every candidate tile was all-land


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Build the highest-zoom terrain tiles from downloaded DEM source rasters."
    )
    parser.add_argument("--dataset", required=True, metavar="NAME",
                        help="Dataset the source rasters came from (see terrain_datasets.py)")
    parser.add_argument("--zoom", required=True, metavar="MIN-MAX",
                        help="Zoom range to build. Only the maximum is used by this step "
                             "(the minimum is for the pyramid-reduction step that runs on top of it).")
    parser.add_argument("--force", action="store_true",
                        help="Rebuild a tile even if its output file already exists")
    parser.add_argument("--no-water", action="store_true",
                        help="Skip building the water/ mask sidecar tree")
    parser.add_argument("--workers", type=int, default=os.cpu_count(),
                        help=f"Number of parallel workers (default: {os.cpu_count()})")
    parser.add_argument("source_dir", help="Directory of downloaded source rasters")
    parser.add_argument("output_dir", help="Directory to write the z/x/y.png tile tree into")
    args = parser.parse_args(argv)

    adapter = get_adapter(args.dataset)

    existing = terrain_manifest.read_manifest(args.output_dir)
    if existing is not None and existing["dataset"] != adapter.name:
        raise ValueError(
            f"{args.output_dir} already holds a {existing['dataset']!r} tile tree; "
            f"refusing to mix {adapter.name!r} tiles into it. Build each dataset into its own directory."
        )

    min_zoom, max_zoom = parse_zoom_range(args.zoom)
    if min_zoom > max_zoom:
        raise ValueError(f"--zoom {args.zoom}: min ({min_zoom}) > max ({max_zoom})")

    index = build_source_index(args.source_dir, adapter)
    if not index:
        raise ValueError(f"no source rasters found in {args.source_dir} for dataset {adapter.name!r}")
    print(f"Indexed {len(index)} source raster(s) from {args.source_dir}/")

    output_pixel_m = (WORLD_CIRCUMFERENCE_M / (2 ** max_zoom)) / terrain_common.TILE_PIXELS
    if output_pixel_m < adapter.native_post_spacing_m * 0.5:
        raise ValueError(
            f"--zoom {max_zoom} asks for a {output_pixel_m:.0f} m output pixel, finer than "
            f"half of {adapter.name}'s {adapter.native_post_spacing_m:.0f} m native post spacing. "
            "This step aggregates source posts into output pixels; it does not interpolate a "
            "coarser source up to a finer zoom. Use a coarser --zoom or a finer-resolution dataset."
        )

    # Validate datum conversion before workers start.
    terrain_datum.verify_datum_conversion(adapter.native_vertical_datum)

    tiles = set()
    for _, (lon_min, lat_min, lon_max, lat_max) in index:
        tiles |= tiles_covering_bounds(lon_min, lat_min, lon_max, lat_max, max_zoom)
    tiles = sorted(tiles)
    print(f"z{max_zoom}: {len(tiles)} candidate tile(s) from source coverage")

    tile_args = [(z, x, y, args.output_dir, args.force) for z, x, y in tiles]
    num_workers = max(1, args.workers)

    start = time.time()
    results = _run_pooled(tile_args, _build_and_report, _worker_init, (args.dataset, index), num_workers)
    elapsed = time.time() - start
    print(f"\n{results.count('built')} built, {results.count('skipped')} skipped (already present), "
          f"{results.count('empty')} empty (no coverage), of {len(tiles)} candidates, in {elapsed:.1f}s")

    # Build and stitch each lower zoom level.
    with pyramid_pool(args.dataset, num_workers) as pool:
        for parent_zoom in range(max_zoom - 1, min_zoom - 1, -1):
            start = time.time()
            built, skipped, empty = build_pyramid_level(parent_zoom, args.output_dir, pool, force=args.force)
            stitched = stitch_level_skirts(parent_zoom, args.output_dir, pool)
            elapsed = time.time() - start
            print(f"z{parent_zoom} (pooled from z{parent_zoom + 1}): {built} built, {skipped} skipped, "
                  f"{empty} empty, {stitched} skirt(s) stitched, in {elapsed:.1f}s")

    # Water-mask sidecar: Copernicus WBM, or rasterised GSHHG for GMTED.
    water = None
    if not args.no_water:
        water = build_water_pass(args.source_dir, adapter, args.output_dir,
                                 min_zoom, max_zoom, tiles, num_workers, force=args.force)

    # Derive the manifest from tiles on disk.
    built_min, built_max, bbox = scan_built_coverage(args.output_dir)
    water_mask = None
    if water is not None:
        water_min, water_max, water_bbox = water
        water_mask = {
            "source": adapter.water_mask_source,
            "classes": terrain_common.WATER_CLASS_NAMES,
            "min_zoom": water_min,
            "max_zoom": water_max,
            "bbox": list(water_bbox),
        }
    terrain_manifest.write_manifest(adapter, args.output_dir, built_min, built_max, bbox,
                                    water_mask=water_mask)
    print(f"\nWrote manifest.json: z{built_min}-{built_max}, "
          f"bbox=({bbox[0]:.2f}, {bbox[1]:.2f}, {bbox[2]:.2f}, {bbox[3]:.2f})"
          + (f", water mask z{water_mask['min_zoom']}-{water_mask['max_zoom']}" if water_mask else ""))


if __name__ == "__main__":
    main()
