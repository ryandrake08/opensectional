#!/usr/bin/env python3
"""Tests for build_terrain.py."""

import contextlib
import glob
import io
import json
import math
import os
import sys
import tempfile
import zipfile
from contextlib import redirect_stdout

import numpy as np
import rasterio
from rasterio.transform import from_origin

import build_terrain
import terrain_common
import terrain_datasets
import terrain_datum
from build_terrain import (
    _neighbor_xy,
    _write_tile,
    all_tiles_at,
    build_pyramid_level,
    build_source_index,
    build_tile,
    build_water_pass,
    candidate_parents,
    expanded_tile_bounds_3857,
    lonlat_to_tile_xy,
    main as run_build_terrain,
    mercator_bounds_to_lonlat,
    parse_zoom_range,
    pool_children,
    pool_tile,
    relevant_sources,
    scan_built_coverage,
    stitch_level_skirts,
    stitch_skirt,
    tile_bounds_3857,
    tiles_covering_bounds,
)
from terrain_datasets import DatasetAdapter


class _FakeAdapter(DatasetAdapter):
    """Test adapter with local GeoTIFF sources."""

    name = "faketest_terrain_adapter"
    display_name = "Fake test dataset"
    attribution = "test fixture, not real data"
    native_vertical_datum = "EGM2008"  # identity by default
    native_nodata = -9999.0
    is_surface_model = False
    native_post_spacing_m = 10.0  # small: any zoom used in tests passes the resolution guard
    vertical_precision_m = terrain_common.VERTICAL_PRECISION_SUBMETRE

    def source_paths(self, source_dir):
        return sorted(glob.glob(os.path.join(source_dir, "*.tif")))


@contextlib.contextmanager
def _patched(obj, name, value):
    original = getattr(obj, name)
    setattr(obj, name, value)
    try:
        yield
    finally:
        setattr(obj, name, original)


@contextlib.contextmanager
def _registered_dataset(name, adapter):
    """Temporarily register a test adapter."""
    terrain_datasets.DATASETS[name] = adapter
    try:
        yield
    finally:
        del terrain_datasets.DATASETS[name]


def _write_source_tif(path, lon0, lat0, size_deg, pixels, values, nodata, dtype="float64"):
    transform = from_origin(lon0, lat0 + size_deg, size_deg / pixels, size_deg / pixels)
    with rasterio.open(
        path, "w", driver="GTiff", height=pixels, width=pixels, count=1,
        dtype=dtype, crs="EPSG:4326", transform=transform, nodata=nodata,
    ) as dataset:
        dataset.write(values, 1)


# --- pure geometry -----------------------------------------------------

def test_parse_zoom_range():
    assert parse_zoom_range("0-6") == (0, 6)
    assert parse_zoom_range("5") == (5, 5)
    return True


def test_tile_bounds_3857_whole_world():
    xmin, ymin, xmax, ymax = tile_bounds_3857(0, 0, 0)
    half = build_terrain.WORLD_CIRCUMFERENCE_M / 2
    assert math.isclose(xmin, -half) and math.isclose(xmax, half)
    assert math.isclose(ymin, -half) and math.isclose(ymax, half)
    return True


def test_expanded_tile_bounds_widens_by_skirt_fraction():
    z, x, y = 4, 3, 5
    xmin, ymin, xmax, ymax = tile_bounds_3857(z, x, y)
    exmin, eymin, exmax, eymax = expanded_tile_bounds_3857(z, x, y)
    px = (xmax - xmin) / terrain_common.TILE_PIXELS
    expected_margin = px * terrain_common.SKIRT_PIXELS
    assert math.isclose(xmin - exmin, expected_margin)
    assert math.isclose(exmax - xmax, expected_margin)
    return True


def test_mercator_lonlat_roundtrip():
    half = build_terrain.WORLD_CIRCUMFERENCE_M / 2
    lon_min, lat_min, lon_max, lat_max = mercator_bounds_to_lonlat(-half, -half, half, half)
    assert math.isclose(lon_min, -180.0, abs_tol=1e-6)
    assert math.isclose(lon_max, 180.0, abs_tol=1e-6)
    assert math.isclose(lat_min, -85.0511287798066, abs_tol=1e-6)
    assert math.isclose(lat_max, 85.0511287798066, abs_tol=1e-6)

    lon_min0, lat_min0, lon_max0, lat_max0 = mercator_bounds_to_lonlat(0, 0, 0, 0)
    assert math.isclose(lon_min0, 0.0, abs_tol=1e-9)
    assert math.isclose(lat_min0, 0.0, abs_tol=1e-9)
    return True


def test_lonlat_to_tile_xy_known_points():
    assert lonlat_to_tile_xy(-180.0, 85.0, 0) == (0, 0)
    assert lonlat_to_tile_xy(0.0, 0.0, 1) == (1, 1)  # centre of the world at z1 -> tile (1,1) of 2x2
    x, y = lonlat_to_tile_xy(179.9, -84.9, 2)
    assert x == 3 and y == 3  # far SE corner at z2 (4x4 grid) -> last tile
    return True


def test_tiles_covering_bounds_small_case():
    # A small bbox squarely inside the NW quadrant at z=1 should give
    # exactly tile (1, 0, 0).
    tiles = tiles_covering_bounds(-90.0, 40.0, -80.0, 50.0, 1)
    assert tiles == {(1, 0, 0)}
    return True


def test_tiles_covering_bounds_empty_beyond_mercator_limit():
    tiles = tiles_covering_bounds(-1.0, 89.0, 1.0, 89.5, 3)  # entirely beyond MERCATOR_MAX_LAT
    assert tiles == set()
    return True


# --- source index --------------------------------------------------------

def test_build_source_index_and_relevant_sources():
    adapter = _FakeAdapter()
    with tempfile.TemporaryDirectory() as d:
        _write_source_tif(os.path.join(d, "a.tif"), 0.0, 0.0, 1.0, 10,
                          np.zeros((10, 10)), -9999.0)
        _write_source_tif(os.path.join(d, "b.tif"), 10.0, 0.0, 1.0, 10,
                          np.zeros((10, 10)), -9999.0)

        index = build_source_index(d, adapter)
        assert len(index) == 2

        hits = relevant_sources(index, (0.2, 0.2, 0.8, 0.8))
        assert len(hits) == 1 and hits[0].endswith("a.tif")

        hits_none = relevant_sources(index, (50.0, 50.0, 51.0, 51.0))
        assert hits_none == []
    return True


# --- build_tile ------------------------------------------------------------

def test_build_tile_aggregates_max_and_propagates_nodata():
    adapter = _FakeAdapter()
    size_deg = 0.2
    pixels = 400
    values = np.full((pixels, pixels), 1000.0)
    # A peak block, comfortably larger than one output pixel at z=8.
    values[50:90, 50:90] = 9999.0
    # A no-data hole in a different corner.
    values[300:360, 300:360] = adapter.native_nodata

    with tempfile.TemporaryDirectory() as source_dir, tempfile.TemporaryDirectory() as output_dir:
        _write_source_tif(os.path.join(source_dir, "patch.tif"), 0.0, 0.0, size_deg, pixels,
                          values, adapter.native_nodata)
        index = build_source_index(source_dir, adapter)

        z = 8
        x, y = lonlat_to_tile_xy(size_deg / 2, size_deg / 2, z)
        result = build_tile(z, x, y, adapter, index, output_dir)
        assert result == "built"

        tile_path = os.path.join(output_dir, str(z), str(x), f"{y}.png")
        assert os.path.exists(tile_path)
        decoded = terrain_common.decode_elevation_array(terrain_common.load_tile_png(tile_path))

        assert np.nanmax(decoded) == 9999.0
        assert np.any(np.isnan(decoded)), "no-data hole should propagate to at least one output pixel"
        assert np.any(decoded == 1000.0), "background value should also survive somewhere"
    return True


def test_build_tile_empty_when_no_coverage():
    adapter = _FakeAdapter()
    with tempfile.TemporaryDirectory() as source_dir, tempfile.TemporaryDirectory() as output_dir:
        _write_source_tif(os.path.join(source_dir, "patch.tif"), 0.0, 0.0, 0.2, 50,
                          np.full((50, 50), 500.0), adapter.native_nodata)
        index = build_source_index(source_dir, adapter)

        # Tile on the opposite side of the world: no source overlap at all.
        z = 8
        x, y = lonlat_to_tile_xy(-150.0, -60.0, z)
        result = build_tile(z, x, y, adapter, index, output_dir)
        assert result == "empty"
        assert not os.path.exists(os.path.join(output_dir, str(z), str(x), f"{y}.png"))
    return True


def test_build_tile_raises_when_reproject_drops_real_data():
    """Reprojection must retain source data."""
    adapter = _FakeAdapter()
    with tempfile.TemporaryDirectory() as source_dir, tempfile.TemporaryDirectory() as output_dir:
        _write_source_tif(os.path.join(source_dir, "patch.tif"), 0.0, 0.0, 0.2, 50,
                          np.full((50, 50), 777.0), adapter.native_nodata)
        index = build_source_index(source_dir, adapter)
        z = 8
        x, y = lonlat_to_tile_xy(0.1, 0.1, z)

        def fake_reproject(**kwargs):
            pass  # leaves `destination` as all-NaN despite real source input

        with _patched(build_terrain, "reproject", fake_reproject):
            try:
                build_tile(z, x, y, adapter, index, output_dir)
                assert False, "expected a RuntimeError"
            except RuntimeError as e:
                assert "reprojection failure" in str(e)
        assert not os.path.exists(os.path.join(output_dir, str(z), str(x), f"{y}.png"))
    return True


def test_build_tile_all_nodata_source_is_empty():
    adapter = _FakeAdapter()
    with tempfile.TemporaryDirectory() as source_dir, tempfile.TemporaryDirectory() as output_dir:
        _write_source_tif(os.path.join(source_dir, "patch.tif"), 0.0, 0.0, 0.2, 50,
                          np.full((50, 50), adapter.native_nodata), adapter.native_nodata)
        index = build_source_index(source_dir, adapter)

        z = 8
        x, y = lonlat_to_tile_xy(0.1, 0.1, z)
        assert build_tile(z, x, y, adapter, index, output_dir) == "empty"
    return True


def test_build_tile_handles_a_source_without_a_nodata_sentinel():
    """A dataset like Copernicus declares native_nodata = None -- its
    COGs carry no sentinel. build_tile must not attempt the `== None`
    substitution and must still produce a tile from the real values."""
    class _NoNodataAdapter(_FakeAdapter):
        name = "faketest_no_nodata_adapter"
        native_nodata = None

    adapter = _NoNodataAdapter()
    with tempfile.TemporaryDirectory() as source_dir, tempfile.TemporaryDirectory() as output_dir:
        _write_source_tif(os.path.join(source_dir, "patch.tif"), 0.0, 0.0, 0.4, 200,
                          np.full((200, 200), 640.0), nodata=None)
        index = build_source_index(source_dir, adapter)

        z = 8
        x, y = lonlat_to_tile_xy(0.1, 0.1, z)
        assert build_tile(z, x, y, adapter, index, output_dir) == "built"

        tile_path = os.path.join(output_dir, str(z), str(x), f"{y}.png")
        elevation = terrain_common.decode_elevation_array(terrain_common.load_tile_png(tile_path))
        interior = terrain_common.extract_interior(elevation)
        # The source values survived; where the tile extends past the
        # small patch, a nodata-less merge fills 0 (ocean-style), which
        # the future water mask is responsible for, not this step.
        assert 640.0 in np.unique(interior)
        assert not np.any(np.isnan(interior))
    return True


class _WbmFakeAdapter(_FakeAdapter):
    name = "faketest_wbm_adapter"
    water_mask_source = "copernicus-wbm"

    def water_source_paths(self, source_dir):
        return sorted(glob.glob(os.path.join(source_dir, "*_WBM.tif")))


def test_build_water_pass_builds_sidecar_and_pyramid():
    """A WBM uint8 source produces a water/ tree whose tiles decode back
    to the source classes, plus a pooled pyramid."""
    adapter = _WbmFakeAdapter()
    with tempfile.TemporaryDirectory() as source_dir, tempfile.TemporaryDirectory() as output_dir:
        pixels = 400
        classes = np.zeros((pixels, pixels), dtype=np.uint8)
        classes[:, :pixels // 2] = terrain_common.WATER_OCEAN     # west half ocean
        classes[100:140, 250:290] = terrain_common.WATER_LAKE     # a lake blob on land
        _write_source_tif(os.path.join(source_dir, "patch_WBM.tif"), 0.0, 0.0, 0.4, pixels,
                          classes, nodata=0, dtype="uint8")

        z = 9
        x, y = lonlat_to_tile_xy(0.2, 0.2, z)
        height_tiles = sorted(tiles_covering_bounds(0.0, 0.0, 0.4, 0.4, z))
        result = build_water_pass(source_dir, adapter, output_dir, z - 2, z, height_tiles, 1)
        assert result == (z - 2, z, result[2])

        water = terrain_common.decode_water_array(
            terrain_common.load_tile_png(os.path.join(output_dir, "water", str(z), str(x), f"{y}.png")))
        interior = terrain_common.extract_interior(water)
        present = set(np.unique(interior).tolist())
        assert terrain_common.WATER_OCEAN in present and terrain_common.WATER_LAKE in present
        assert interior[interior.shape[0] // 2, 0] == terrain_common.WATER_OCEAN     # west: ocean
        assert interior[interior.shape[0] // 2, -1] == terrain_common.WATER_NONE     # east: land

        assert all_tiles_at(z - 1, os.path.join(output_dir, "water"))
        parent = terrain_common.decode_water_array(terrain_common.load_tile_png(
            os.path.join(output_dir, "water", str(z - 1), str(x // 2), f"{y // 2}.png")))
        assert (parent != 0).any()
    return True


def test_build_water_pass_builds_to_height_max_zoom():
    """The water tree is built to the same max zoom as the height tree
    (no cap), so the client always finds an exact-zoom water tile."""
    adapter = _WbmFakeAdapter()
    with tempfile.TemporaryDirectory() as source_dir, tempfile.TemporaryDirectory() as output_dir:
        _write_source_tif(os.path.join(source_dir, "p_WBM.tif"), 0.0, 0.0, 0.4, 100,
                          np.full((100, 100), terrain_common.WATER_OCEAN, dtype=np.uint8),
                          nodata=0, dtype="uint8")
        height_zoom = 13
        height_tiles = sorted(tiles_covering_bounds(0.0, 0.0, 0.4, 0.4, height_zoom))
        result = build_water_pass(source_dir, adapter, output_dir, height_zoom - 1, height_zoom,
                                  height_tiles, 1)
        assert result[1] == height_zoom
        assert all_tiles_at(height_zoom, os.path.join(output_dir, "water"))
    return True


def test_build_water_pass_returns_none_without_water_sources():
    with tempfile.TemporaryDirectory() as source_dir, tempfile.TemporaryDirectory() as output_dir:
        assert build_water_pass(source_dir, _FakeAdapter(), output_dir, 0, 4, [], 1) is None
    return True


def test_gshhg_water_provider_rasterizes_a_shoreline():
    """A minimal GSHHG-layout zip with one land polygon: the provider
    returns ocean outside it and land inside."""
    from build_terrain import _GshhgWaterProvider
    import fiona
    from shapely.geometry import mapping, box as _box

    with tempfile.TemporaryDirectory() as d:
        shp_dir = os.path.join(d, "GSHHS_shp", "i")
        os.makedirs(shp_dir)
        land = _box(-1.0, -1.0, 0.5, 0.5)  # covers the SW, leaves NE as ocean
        schema = {"geometry": "Polygon", "properties": {"level": "int"}}
        with fiona.open(os.path.join(shp_dir, "GSHHS_i_L1.shp"), "w",
                        driver="ESRI Shapefile", crs="EPSG:4326", schema=schema) as dst:
            dst.write({"geometry": mapping(land), "properties": {"level": 1}})
        zip_path = os.path.join(d, "gshhg.zip")
        with zipfile.ZipFile(zip_path, "w") as zf:
            for f in os.listdir(shp_dir):
                zf.write(os.path.join(shp_dir, f), f"GSHHS_shp/i/{f}")

        provider = _GshhgWaterProvider(zip_path, "i")
        z = 9
        land_tile = lonlat_to_tile_xy(-0.5, -0.5, z)
        ocean_tile = lonlat_to_tile_xy(3.0, 3.0, z)
        land_classes = provider(z, *land_tile)
        ocean_classes = provider(z, *ocean_tile)
        assert (land_classes == terrain_common.WATER_NONE).mean() > 0.9
        assert np.all(ocean_classes == terrain_common.WATER_OCEAN)
    return True


def test_build_tile_skips_existing_unless_force():
    adapter = _FakeAdapter()
    with tempfile.TemporaryDirectory() as source_dir, tempfile.TemporaryDirectory() as output_dir:
        _write_source_tif(os.path.join(source_dir, "patch.tif"), 0.0, 0.0, 0.2, 50,
                          np.full((50, 50), 123.0), adapter.native_nodata)
        index = build_source_index(source_dir, adapter)

        z = 8
        x, y = lonlat_to_tile_xy(0.1, 0.1, z)
        assert build_tile(z, x, y, adapter, index, output_dir) == "built"
        assert build_tile(z, x, y, adapter, index, output_dir) == "skipped"
        assert build_tile(z, x, y, adapter, index, output_dir, force=True) == "built"
    return True


def test_build_tile_applies_datum_conversion():
    """Confirms build_tile threads elevation through terrain_datum with
    the adapter's declared native_vertical_datum, via a deterministic
    stand-in rather than the real (network-fetched) geoid grids."""
    adapter = _FakeAdapter()
    adapter.native_vertical_datum = "FAKE_DATUM"

    with tempfile.TemporaryDirectory() as source_dir, tempfile.TemporaryDirectory() as output_dir:
        _write_source_tif(os.path.join(source_dir, "patch.tif"), 0.0, 0.0, 0.2, 50,
                          np.full((50, 50), 100.0), adapter.native_nodata)
        index = build_source_index(source_dir, adapter)
        z = 8
        x, y = lonlat_to_tile_xy(0.1, 0.1, z)

        def fake_to_egm2008(elevation, lon, lat, datum):
            assert datum == "FAKE_DATUM"
            return elevation + 5.0

        with _patched(terrain_datum, "to_egm2008", fake_to_egm2008):
            assert build_tile(z, x, y, adapter, index, output_dir) == "built"

        tile_path = os.path.join(output_dir, str(z), str(x), f"{y}.png")
        decoded = terrain_common.decode_elevation_array(terrain_common.load_tile_png(tile_path))
        assert np.nanmax(decoded) == 105.0
    return True


# --- pyramid reduction -----------------------------------------------

def _full_array(interior=1.0, skirt=np.nan):
    arr = np.full((terrain_common.FULL_TILE_PIXELS, terrain_common.FULL_TILE_PIXELS), skirt)
    rows, cols = terrain_common.interior_slice()
    arr[rows, cols] = interior
    return arr


def test_neighbor_xy_wraps_longitude_not_latitude():
    n = 4
    assert _neighbor_xy(0, 1, "W", n) == (3, 1)  # wraps around the antimeridian
    assert _neighbor_xy(3, 1, "E", n) == (0, 1)
    assert _neighbor_xy(1, 0, "N", n) is None  # off the top of the world
    assert _neighbor_xy(1, n - 1, "S", n) is None  # off the bottom
    assert _neighbor_xy(0, 0, "NW", n) is None
    return True


def test_candidate_parents_scans_disk():
    adapter = _FakeAdapter()
    with tempfile.TemporaryDirectory() as output_dir:
        for x, y in [(2, 4), (3, 4), (2, 5), (10, 10)]:
            _write_tile(os.path.join(output_dir, "6", str(x), f"{y}.png"), _full_array(1.0), adapter)
        assert candidate_parents(6, output_dir) == {(1, 2), (5, 5)}
        assert candidate_parents(9, output_dir) == set()  # nothing built at that level
    return True


def test_tile_scans_ignore_interrupted_write_artifacts():
    adapter = _FakeAdapter()
    with tempfile.TemporaryDirectory() as output_dir:
        _write_tile(os.path.join(output_dir, "6", "2", "4.png"), _full_array(1.0), adapter)
        with open(os.path.join(output_dir, "6", "2", "5.tmp.png"), "wb"):
            pass
        with open(os.path.join(output_dir, "6", ".DS_Store"), "wb"):
            pass

        assert candidate_parents(6, output_dir) == {(1, 2)}
        assert all_tiles_at(6, output_dir) == [(6, 2, 4)]
    return True


def test_pool_children_max_and_missing_child():
    adapter = _FakeAdapter()
    with tempfile.TemporaryDirectory() as output_dir:
        # Three of four children present; NW child holds a peak. The
        # SE child (11, 21) is left missing entirely.
        _write_tile(os.path.join(output_dir, "6", "10", "20.png"), _full_array(500.0), adapter)  # NW
        _write_tile(os.path.join(output_dir, "6", "11", "20.png"), _full_array(100.0), adapter)  # NE
        _write_tile(os.path.join(output_dir, "6", "10", "21.png"), _full_array(100.0), adapter)  # SW

        pooled = pool_children(5, 5, 10, output_dir)
        assert pooled is not None
        assert np.nanmax(pooled) == 500.0

        # A missing child means its whole quadrant of the pooled result
        # is legitimately no-data -- only the three present children's
        # quadrants should be populated.
        half = terrain_common.TILE_PIXELS // 2
        assert not np.any(np.isnan(pooled[:half, :half])), "NW quadrant (present) should be populated"
        assert not np.any(np.isnan(pooled[:half, half:])), "NE quadrant (present) should be populated"
        assert not np.any(np.isnan(pooled[half:, :half])), "SW quadrant (present) should be populated"
        assert np.all(np.isnan(pooled[half:, half:])), "SE quadrant (missing child) should be all no-data"
    return True


def test_pool_children_all_missing_is_none():
    with tempfile.TemporaryDirectory() as output_dir:
        assert pool_children(5, 5, 10, output_dir) is None
    return True


def test_pool_tile_writes_correct_interior_and_is_resumable():
    adapter = _FakeAdapter()
    with tempfile.TemporaryDirectory() as output_dir:
        for x, y in [(10, 20), (11, 20), (10, 21), (11, 21)]:
            _write_tile(os.path.join(output_dir, "6", str(x), f"{y}.png"), _full_array(42.0), adapter)

        assert pool_tile(5, 5, 10, adapter, output_dir) == "built"
        tile_path = os.path.join(output_dir, "5", "5", "10.png")
        decoded = terrain_common.decode_elevation_array(terrain_common.load_tile_png(tile_path))
        assert np.all(terrain_common.extract_interior(decoded) == 42.0)

        assert pool_tile(5, 5, 10, adapter, output_dir) == "skipped"
        assert pool_tile(5, 5, 10, adapter, output_dir, force=True) == "built"
    return True


def test_pool_tile_empty_when_no_children():
    adapter = _FakeAdapter()
    with tempfile.TemporaryDirectory() as output_dir:
        assert pool_tile(5, 5, 10, adapter, output_dir) == "empty"
    return True


def test_stitch_skirt_pulls_real_neighbor_data():
    adapter = _FakeAdapter()
    with tempfile.TemporaryDirectory() as output_dir:
        # Two east-west same-zoom neighbours, distinct interior values.
        _write_tile(os.path.join(output_dir, "6", "10", "20.png"), _full_array(111.0), adapter)
        _write_tile(os.path.join(output_dir, "6", "11", "20.png"), _full_array(222.0), adapter)

        stitch_skirt(6, 10, 20, adapter, output_dir)

        tile_path = os.path.join(output_dir, "6", "10", "20.png")
        decoded = terrain_common.decode_elevation_array(terrain_common.load_tile_png(tile_path))
        s = terrain_common.SKIRT_PIXELS
        assert np.all(decoded[s:-s, -s:] == 222.0), "east skirt should hold the east neighbour's edge"
        assert np.all(np.isnan(decoded[s:-s, :s])), "no west neighbour exists -- west skirt stays no-data"
        assert np.all(terrain_common.extract_interior(decoded) == 111.0), "interior must be untouched"
    return True


def test_stitch_skirt_no_neighbors_leaves_skirt_as_nodata():
    adapter = _FakeAdapter()
    with tempfile.TemporaryDirectory() as output_dir:
        _write_tile(os.path.join(output_dir, "4", "5", "5.png"), _full_array(9.0), adapter)
        stitch_skirt(4, 5, 5, adapter, output_dir)
        decoded = terrain_common.decode_elevation_array(
            terrain_common.load_tile_png(os.path.join(output_dir, "4", "5", "5.png"))
        )
        rows, cols = terrain_common.interior_slice()
        skirt_mask = np.ones_like(decoded, dtype=bool)
        skirt_mask[rows, cols] = False
        assert np.all(np.isnan(decoded[skirt_mask]))
        assert np.all(decoded[rows, cols] == 9.0)
    return True


def test_all_tiles_at():
    adapter = _FakeAdapter()
    with tempfile.TemporaryDirectory() as output_dir:
        for x, y in [(1, 2), (1, 3), (4, 4)]:
            _write_tile(os.path.join(output_dir, "3", str(x), f"{y}.png"), _full_array(1.0), adapter)
        assert set(all_tiles_at(3, output_dir)) == {(3, 1, 2), (3, 1, 3), (3, 4, 4)}
        assert all_tiles_at(9, output_dir) == []
    return True


def test_scan_built_coverage_reconstructs_the_built_tile_set():
    """The derived bbox is correct if feeding it back through
    tiles_covering_bounds reproduces exactly the tiles that were
    actually built -- a precise, non-hand-derived check."""
    adapter = _FakeAdapter()
    with tempfile.TemporaryDirectory() as output_dir:
        built = {(6, 10, 20), (6, 11, 20), (6, 10, 21), (6, 11, 21)}
        for z, x, y in built:
            _write_tile(os.path.join(output_dir, str(z), str(x), f"{y}.png"), _full_array(1.0), adapter)

        min_zoom, max_zoom, bbox = scan_built_coverage(output_dir)
        assert (min_zoom, max_zoom) == (6, 6)
        reconstructed = tiles_covering_bounds(*bbox, 6)
        assert reconstructed == built
    return True


def test_scan_built_coverage_raises_when_nothing_built():
    with tempfile.TemporaryDirectory() as output_dir:
        try:
            scan_built_coverage(output_dir)
            assert False, "expected a ValueError"
        except ValueError as e:
            assert "no built tiles" in str(e)
    return True


def test_scan_built_coverage_warns_on_a_gap():
    adapter = _FakeAdapter()
    with tempfile.TemporaryDirectory() as output_dir:
        _write_tile(os.path.join(output_dir, "8", "1", "1.png"), _full_array(1.0), adapter)
        _write_tile(os.path.join(output_dir, "6", "0", "0.png"), _full_array(1.0), adapter)
        # z7 deliberately missing -- a gap in the pyramid.

        captured = io.StringIO()
        with redirect_stdout(captured):
            min_zoom, max_zoom, _ = scan_built_coverage(output_dir)
        assert (min_zoom, max_zoom) == (6, 8)
        assert "gap" in captured.getvalue().lower()
    return True


def test_build_pyramid_level_and_stitch_via_registry():
    """Pyramid workers resolve adapters from the registry."""
    adapter = _FakeAdapter()
    with tempfile.TemporaryDirectory() as output_dir, _registered_dataset("faketest_terrain_adapter", adapter):
        for x, y in [(10, 20), (11, 20), (10, 21), (11, 21)]:
            _write_tile(os.path.join(output_dir, "6", str(x), f"{y}.png"), _full_array(7.0), adapter)

        with build_terrain.pyramid_pool("faketest_terrain_adapter", num_workers=1) as pool:
            built, skipped, empty = build_pyramid_level(5, output_dir, pool)
            assert (built, skipped, empty) == (1, 0, 0)
            assert stitch_level_skirts(5, output_dir, pool) == 1
    return True


def test_main_builds_full_multilevel_pyramid():
    """Build, pool, stitch, and describe a multilevel pyramid."""
    adapter = _FakeAdapter()
    with tempfile.TemporaryDirectory() as source_dir, tempfile.TemporaryDirectory() as output_dir:
        size_deg, pixels = 8.0, 800
        values = np.full((pixels, pixels), 1000.0)
        values[400:420, 400:420] = 9999.0  # a peak near the centre of the patch
        _write_source_tif(os.path.join(source_dir, "patch.tif"), 0.0, 0.0, size_deg, pixels,
                          values, adapter.native_nodata)

        with _registered_dataset("faketest_terrain_adapter", adapter):
            argv = ["--dataset", "faketest_terrain_adapter", "--zoom", "6-8", "--workers", "1",
                   source_dir, output_dir]
            run_build_terrain(argv)

        z6_tiles = all_tiles_at(6, output_dir)
        z7_tiles = all_tiles_at(7, output_dir)
        z8_tiles = all_tiles_at(8, output_dir)
        assert len(z6_tiles) == 4 and len(z7_tiles) == 9 and len(z8_tiles) > len(z7_tiles)

        # The peak must survive two full levels of max-pooling down to z6.
        z6_max = max(
            np.nanmax(terrain_common.decode_elevation_array(
                terrain_common.load_tile_png(os.path.join(output_dir, str(z), str(x), f"{y}.png"))))
            for z, x, y in z6_tiles
        )
        assert z6_max == 9999.0

        # The centre tile of the z7 3x3 grid has all 8 same-zoom
        # neighbours built, so pyramid-level stitching should leave no
        # no-data anywhere in its skirt.
        xs = sorted({x for _, x, _ in z7_tiles})
        ys = sorted({y for _, _, y in z7_tiles})
        center_x, center_y = xs[1], ys[1]
        decoded = terrain_common.decode_elevation_array(
            terrain_common.load_tile_png(os.path.join(output_dir, "7", str(center_x), f"{center_y}.png"))
        )
        rows, cols = terrain_common.interior_slice()
        skirt_mask = np.ones_like(decoded, dtype=bool)
        skirt_mask[rows, cols] = False
        assert not np.any(np.isnan(decoded[skirt_mask])), \
            "the z7 grid's centre tile has every neighbour built; its skirt should be fully populated"

        # main() writes manifest.json at the end of the run, derived
        # from what was actually built -- z6 (the requested minimum),
        # not z3 or anything else, and a bbox that reconstructs the
        # exact z8 tile set through tiles_covering_bounds.
        with open(os.path.join(output_dir, "manifest.json")) as f:
            manifest = json.load(f)
        assert manifest["dataset"] == "faketest_terrain_adapter"
        assert manifest["min_zoom"] == 6
        assert manifest["max_zoom"] == 8
        assert manifest["vertical_datum"] == "EGM2008"
        lon_min, lat_min, lon_max, lat_max = manifest["bbox"]
        assert set(tiles_covering_bounds(lon_min, lat_min, lon_max, lat_max, 8)) == set(z8_tiles)
    return True


# --- main() CLI ------------------------------------------------------------

def test_main_end_to_end():
    adapter = _FakeAdapter()
    with tempfile.TemporaryDirectory() as source_dir, tempfile.TemporaryDirectory() as output_dir:
        _write_source_tif(os.path.join(source_dir, "patch.tif"), 0.0, 0.0, 0.2, 100,
                          np.full((100, 100), 250.0), adapter.native_nodata)

        with _registered_dataset("faketest_terrain_adapter", adapter):
            argv = ["--dataset", "faketest_terrain_adapter", "--zoom", "8", "--workers", "1",
                   source_dir, output_dir]
            run_build_terrain(argv)

        z = 8
        x, y = lonlat_to_tile_xy(0.1, 0.1, z)
        tile_path = os.path.join(output_dir, str(z), str(x), f"{y}.png")
        assert os.path.exists(tile_path)
    return True


def test_main_rejects_zoom_finer_than_source_resolution():
    adapter = _FakeAdapter()
    adapter.native_post_spacing_m = 100000.0  # absurdly coarse, so any real zoom trips the guard
    with tempfile.TemporaryDirectory() as source_dir, tempfile.TemporaryDirectory() as output_dir:
        _write_source_tif(os.path.join(source_dir, "patch.tif"), 0.0, 0.0, 0.2, 10,
                          np.full((10, 10), 1.0), adapter.native_nodata)

        with _registered_dataset("faketest_terrain_adapter", adapter):
            argv = ["--dataset", "faketest_terrain_adapter", "--zoom", "10", "--workers", "1",
                   source_dir, output_dir]
            try:
                run_build_terrain(argv)
                assert False, "expected ValueError"
            except ValueError as e:
                assert "native post spacing" in str(e)
    return True


def test_main_rejects_a_different_dataset_for_an_existing_tree():
    """A tile tree accepts only its manifest dataset."""
    adapter = _FakeAdapter()
    with tempfile.TemporaryDirectory() as source_dir, tempfile.TemporaryDirectory() as output_dir:
        with open(os.path.join(output_dir, "manifest.json"), "w") as f:
            json.dump({"dataset": "some_other_dataset"}, f)
        _write_source_tif(os.path.join(source_dir, "patch.tif"), 0.0, 0.0, 0.2, 10,
                          np.full((10, 10), 1.0), adapter.native_nodata)

        with _registered_dataset("faketest_terrain_adapter", adapter):
            argv = ["--dataset", "faketest_terrain_adapter", "--zoom", "8", "--workers", "1",
                   source_dir, output_dir]
            try:
                run_build_terrain(argv)
                assert False, "expected ValueError"
            except ValueError as e:
                assert "some_other_dataset" in str(e)
    return True


TESTS = [
    test_parse_zoom_range,
    test_tile_bounds_3857_whole_world,
    test_expanded_tile_bounds_widens_by_skirt_fraction,
    test_mercator_lonlat_roundtrip,
    test_lonlat_to_tile_xy_known_points,
    test_tiles_covering_bounds_small_case,
    test_tiles_covering_bounds_empty_beyond_mercator_limit,
    test_build_source_index_and_relevant_sources,
    test_build_tile_aggregates_max_and_propagates_nodata,
    test_build_tile_empty_when_no_coverage,
    test_build_tile_raises_when_reproject_drops_real_data,
    test_build_tile_all_nodata_source_is_empty,
    test_build_tile_handles_a_source_without_a_nodata_sentinel,
    test_build_water_pass_builds_sidecar_and_pyramid,
    test_build_water_pass_builds_to_height_max_zoom,
    test_build_water_pass_returns_none_without_water_sources,
    test_gshhg_water_provider_rasterizes_a_shoreline,
    test_build_tile_skips_existing_unless_force,
    test_build_tile_applies_datum_conversion,
    test_neighbor_xy_wraps_longitude_not_latitude,
    test_candidate_parents_scans_disk,
    test_tile_scans_ignore_interrupted_write_artifacts,
    test_pool_children_max_and_missing_child,
    test_pool_children_all_missing_is_none,
    test_pool_tile_writes_correct_interior_and_is_resumable,
    test_pool_tile_empty_when_no_children,
    test_stitch_skirt_pulls_real_neighbor_data,
    test_stitch_skirt_no_neighbors_leaves_skirt_as_nodata,
    test_all_tiles_at,
    test_scan_built_coverage_reconstructs_the_built_tile_set,
    test_scan_built_coverage_raises_when_nothing_built,
    test_scan_built_coverage_warns_on_a_gap,
    test_build_pyramid_level_and_stitch_via_registry,
    test_main_builds_full_multilevel_pyramid,
    test_main_end_to_end,
    test_main_rejects_zoom_finer_than_source_resolution,
    test_main_rejects_a_different_dataset_for_an_existing_tree,
]


def main():
    failed = 0
    for test in TESTS:
        name = test.__name__
        try:
            test()
            print(f"PASS  {name}")
        except AssertionError as e:
            failed += 1
            print(f"FAIL  {name}: {e}")
        except Exception as e:  # noqa: BLE001 - report and keep going
            failed += 1
            print(f"ERROR {name}: {type(e).__name__}: {e}")

    total = len(TESTS)
    print(f"\n{total - failed}/{total} passed")
    sys.exit(0 if failed == 0 else 1)


if __name__ == "__main__":
    main()
