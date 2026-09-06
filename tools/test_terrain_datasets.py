#!/usr/bin/env python3
"""Tests for the dataset adapter registry in terrain_datasets.py."""

import os
import sys
import tempfile

import numpy as np
import rasterio
from rasterio.transform import from_origin

from terrain_datasets import (
    DATASETS,
    DatasetAdapter,
    Gmted2010Adapter,
    get_adapter,
)


def test_registry_lookup():
    adapter = get_adapter("gmted2010")
    assert isinstance(adapter, Gmted2010Adapter)
    assert adapter is DATASETS["gmted2010"]
    return True


def test_registry_unknown_dataset():
    try:
        get_adapter("nonexistent")
        assert False, "expected ValueError"
    except ValueError as e:
        assert "gmted2010" in str(e)
    return True


def test_base_class_is_unusable_directly():
    """A concrete adapter that forgets to implement a method fails
    loudly rather than silently doing nothing."""
    base = DatasetAdapter()
    for method, args in (
        ("tiles_for_bbox", (0, 0, 1, 1)),
        ("open_source_raster", ("/nonexistent",)),
    ):
        try:
            getattr(base, method)(*args)
            assert False, f"expected NotImplementedError from {method}"
        except NotImplementedError:
            pass
    return True


def test_declared_metadata():
    """The fields every adapter must declare (D1's registry contract)."""
    a = get_adapter("gmted2010")
    assert a.name == "gmted2010"
    assert a.native_vertical_datum == "EGM96"
    assert a.is_surface_model is True
    assert 900.0 < a.native_post_spacing_m < 950.0  # ~928 m at 30 arcsec
    assert a.vertical_precision_m == 1.0
    assert "public domain" in a.attribution.lower()
    return True


def test_tile_code_matches_known_example():
    """USGS distributes a real file named 30N030W_20101117_gmted_mea300.tif
    -- the SW-corner tile at 30N, 30W. Confirm this adapter's naming
    matches that convention (with our chosen 'max' statistic)."""
    a = get_adapter("gmted2010")
    assert a._tile_code(-30, 30) == "30N030W"
    assert a._tile_filename(-30, 30) == "30N030W_20101117_gmted_max300.tif"
    # Equator/prime-meridian tile: no leading sign digits lost.
    assert a._tile_code(0, 0) == "00N000E"
    assert a._tile_code(150, -90) == "90S150E"
    return True


def test_tiles_for_bbox_single_tile():
    a = get_adapter("gmted2010")
    # Fully inside the (-30, 30)..(0, 50) tile.
    tiles = a.tiles_for_bbox(-20.0, 35.0, -10.0, 45.0)
    assert len(tiles) == 1
    assert tiles[0].bounds == (-30, 30, 0, 50)
    assert tiles[0].local_name == "30N030W_20101117_gmted_max300.tif"
    assert tiles[0].url.endswith(tiles[0].local_name)
    return True


def test_tiles_for_bbox_boundary_is_half_open():
    """A bbox whose max edge lands exactly on a grid line doesn't pull
    in the unneeded neighbouring tile."""
    a = get_adapter("gmted2010")
    tiles = a.tiles_for_bbox(-30.0, 30.0, 0.0, 50.0)
    assert len(tiles) == 1
    assert tiles[0].bounds == (-30, 30, 0, 50)
    return True


def test_tiles_for_bbox_spans_multiple():
    a = get_adapter("gmted2010")
    # Straddles the lon=-30 tile boundary and the lat=30 tile boundary:
    # four tiles, no duplicates.
    tiles = a.tiles_for_bbox(-40.0, 20.0, -20.0, 40.0)
    names = {t.local_name for t in tiles}
    assert len(names) == 4 == len(tiles)
    for t in tiles:
        overlaps_lon = t.bounds[0] < -20.0 and t.bounds[2] > -40.0
        overlaps_lat = t.bounds[1] < 40.0 and t.bounds[3] > 20.0
        assert overlaps_lon and overlaps_lat, t.bounds
    return True


def test_tiles_for_bbox_antimeridian():
    """The Aleutian Islands case: a bbox that crosses +/-180 splits and
    merges without double-counting a shared tile."""
    a = get_adapter("gmted2010")
    tiles = a.tiles_for_bbox(170.0, 50.0, -170.0, 60.0)
    names = [t.local_name for t in tiles]
    assert len(names) == len(set(names)), "duplicate tile across the seam"
    lons = {t.bounds[0] for t in tiles}
    assert any(lon >= 150 for lon in lons), "missing a tile west of the seam"
    assert any(lon <= -150 for lon in lons), "missing a tile east of the seam"
    return True


def test_tiles_for_bbox_usa_sanity():
    """CONUS + Alaska (crossing into the Aleutians) covers a wide swath
    without error, and every returned tile actually overlaps the query."""
    a = get_adapter("gmted2010")
    tiles = a.tiles_for_bbox(172.0, 15.0, -65.0, 72.0)  # wraps at the seam
    assert len(tiles) > 10
    for t in tiles:
        lon_min, lat_min, lon_max, lat_max = t.bounds
        overlaps_west_side = lon_max > 172.0 and lon_min < 180.0
        overlaps_east_side = lon_max > -180.0 and lon_min < -65.0
        assert overlaps_west_side or overlaps_east_side, t.bounds
        assert lat_max > 15.0 and lat_min < 72.0
    return True


def test_open_source_raster_roundtrip():
    """A synthetic GeoTIFF, including a no-data cell, decodes to the
    expected float array with NaN at the sentinel and the transform
    preserved."""
    a = get_adapter("gmted2010")
    values = np.array([[100, 200, -32768], [300, 400, 500]], dtype=np.int16)
    transform = from_origin(-30.0, 30.0, 30.0 / 3600, 30.0 / 3600)

    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "30N030W_20101117_gmted_max300.tif")
        with rasterio.open(
            path, "w", driver="GTiff", height=2, width=3, count=1,
            dtype="int16", crs="EPSG:4326", transform=transform,
            nodata=-32768,
        ) as dataset:
            dataset.write(values, 1)

        raster = a.open_source_raster(path)

    assert raster.elevation_m.shape == (2, 3)
    assert np.isnan(raster.elevation_m[0, 2])
    expected = values.astype(np.float64)
    mask = ~np.isnan(raster.elevation_m)
    assert np.array_equal(raster.elevation_m[mask], expected[mask])
    assert raster.transform == transform
    return True


def test_open_source_raster_without_explicit_nodata_tag():
    """A file that omits the nodata tag falls back to NATIVE_NODATA."""
    a = get_adapter("gmted2010")
    values = np.array([[a.NATIVE_NODATA, 50]], dtype=np.int16)
    transform = from_origin(0.0, 20.0, 30.0 / 3600, 30.0 / 3600)

    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "00N000E_20101117_gmted_max300.tif")
        with rasterio.open(
            path, "w", driver="GTiff", height=1, width=2, count=1,
            dtype="int16", crs="EPSG:4326", transform=transform,
        ) as dataset:
            dataset.write(values, 1)

        raster = a.open_source_raster(path)

    assert np.isnan(raster.elevation_m[0, 0])
    assert raster.elevation_m[0, 1] == 50.0
    return True


TESTS = [
    test_registry_lookup,
    test_registry_unknown_dataset,
    test_base_class_is_unusable_directly,
    test_declared_metadata,
    test_tile_code_matches_known_example,
    test_tiles_for_bbox_single_tile,
    test_tiles_for_bbox_boundary_is_half_open,
    test_tiles_for_bbox_spans_multiple,
    test_tiles_for_bbox_antimeridian,
    test_tiles_for_bbox_usa_sanity,
    test_open_source_raster_roundtrip,
    test_open_source_raster_without_explicit_nodata_tag,
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
