#!/usr/bin/env python3
"""Tests for the dataset adapter registry in terrain_datasets.py."""

import os
import sys
import tempfile
import zipfile

from terrain_datasets import (
    DATASETS,
    CopernicusGloAdapter,
    DatasetAdapter,
    Gmted2010Adapter,
    _cell_overlaps_bbox,
    _parse_copernicus_cell,
    get_adapter,
)


def test_registry_lookup():
    for name in ("gmted2010-30", "gmted2010-15", "gmted2010-75"):
        adapter = get_adapter(name)
        assert isinstance(adapter, Gmted2010Adapter)
        assert adapter is DATASETS[name]
        # adapter.name must round-trip: build_terrain hands args.dataset to
        # worker processes, which re-look-up via get_adapter, and the
        # existing-tree guard compares manifest["dataset"] to adapter.name.
        assert adapter.name == name
    return True


def test_resolution_variants_differ_only_in_resolution():
    r30, r15, r75 = (get_adapter(n) for n in ("gmted2010-30", "gmted2010-15", "gmted2010-75"))

    # Finer resolution -> smaller post spacing, halving each step.
    assert r30.native_post_spacing_m > r15.native_post_spacing_m > r75.native_post_spacing_m
    # arcsec factors are 30/15/7.5, so the spacings differ by exact powers of
    # two: scaling by 2^n commutes with rounding, so these are bit-exact.
    assert r15.native_post_spacing_m == r30.native_post_spacing_m / 2.0
    assert r75.native_post_spacing_m == r30.native_post_spacing_m / 4.0

    # Distinct source archives.
    assert (r30.GRID_ZIP_NAME, r15.GRID_ZIP_NAME, r75.GRID_ZIP_NAME) == (
        "mx30_grd.zip",
        "mx15_grd.zip",
        "mx75_grd.zip",
    )

    # Everything else is shared.
    for a in (r15, r75):
        assert a.source_version == r30.source_version
        assert a.native_vertical_datum == r30.native_vertical_datum
        assert a.is_surface_model == r30.is_surface_model
        assert a.vertical_precision_m == r30.vertical_precision_m
        assert a.attribution == r30.attribution
    return True


def test_registry_unknown_dataset():
    try:
        get_adapter("nonexistent")
        assert False, "expected ValueError"
    except ValueError as e:
        assert "gmted2010-30" in str(e)
    return True


def test_base_class_is_unusable_directly():
    """A concrete adapter that forgets to implement a method fails
    loudly rather than silently doing nothing."""
    base = DatasetAdapter()
    for method, args in (
        ("source_tiles", ()),
        ("source_paths", ("/nonexistent",)),
    ):
        try:
            getattr(base, method)(*args)
            assert False, f"expected NotImplementedError from {method}"
        except NotImplementedError:
            pass
    return True


def test_declared_metadata():
    """The fields every adapter must declare (D1's registry contract)."""
    a = get_adapter("gmted2010-30")
    assert a.name == "gmted2010-30"
    assert a.source_version == "20101117"
    assert a.native_vertical_datum == "EGM96"
    assert a.is_surface_model is True
    assert 900.0 < a.native_post_spacing_m < 950.0  # ~928 m at 30 arcsec
    assert a.vertical_precision_m == 1.0
    assert "public domain" in a.attribution.lower()
    return True


def test_source_tiles_returns_the_global_archive_plus_gshhg():
    a = get_adapter("gmted2010-30")
    assert [t.local_name for t in a.source_tiles()] == [a.GRID_ZIP_NAME, a.GSHHG_ZIP_NAME]
    # water=False drops the shoreline archive.
    water_off = a.source_tiles(water=False)
    assert len(water_off) == 1 and water_off[0].local_name == a.GRID_ZIP_NAME
    assert a.source_tiles()[1].url == a.GSHHG_URL
    return True


def test_gmted_gshhg_zip_path():
    a = get_adapter("gmted2010-15")
    with tempfile.TemporaryDirectory() as source_dir:
        assert a.gshhg_zip_path(source_dir) is None
        p = os.path.join(source_dir, a.GSHHG_ZIP_NAME)
        open(p, "w").close()
        assert a.gshhg_zip_path(source_dir) == p
    return True


def test_source_paths_extracts_the_zip_once():
    """The ArcGrid entries inside the real archive are DEFLATE-compressed,
    which makes GDAL's /vsizip/ virtual filesystem far too slow for
    windowed reads (measured ~500x); source_paths extracts to a plain
    directory instead, and only does so once."""
    a = get_adapter("gmted2010-30")
    with tempfile.TemporaryDirectory() as source_dir:
        zip_path = os.path.join(source_dir, a.GRID_ZIP_NAME)
        with zipfile.ZipFile(zip_path, "w") as zf:
            zf.writestr(f"{a.GRID_INTERNAL_NAME}/hdr.adf", b"fake header bytes")

        extracted_dir = os.path.join(source_dir, a.GRID_INTERNAL_NAME)
        assert a.source_paths(source_dir) == [extracted_dir]
        assert os.path.exists(os.path.join(extracted_dir, "hdr.adf"))

        # Second call reuses the already-extracted directory rather than
        # re-extracting -- confirmed by removing the zip and checking
        # source_paths still succeeds.
        os.remove(zip_path)
        assert a.source_paths(source_dir) == [extracted_dir]
    return True


def test_source_paths_replaces_an_incomplete_extraction():
    a = get_adapter("gmted2010-30")
    with tempfile.TemporaryDirectory() as source_dir:
        zip_path = os.path.join(source_dir, a.GRID_ZIP_NAME)
        with zipfile.ZipFile(zip_path, "w") as zf:
            zf.writestr(f"{a.GRID_INTERNAL_NAME}/hdr.adf", b"complete header")

        extracted_dir = os.path.join(source_dir, a.GRID_INTERNAL_NAME)
        os.makedirs(extracted_dir)
        with open(os.path.join(extracted_dir, "partial.adf"), "wb") as f:
            f.write(b"incomplete")

        assert a.source_paths(source_dir) == [extracted_dir]
        with open(os.path.join(extracted_dir, "hdr.adf"), "rb") as f:
            assert f.read() == b"complete header"
        assert not os.path.exists(os.path.join(extracted_dir, "partial.adf"))
    return True


def test_source_paths_empty_when_not_yet_downloaded():
    a = get_adapter("gmted2010-30")
    with tempfile.TemporaryDirectory() as source_dir:
        assert a.source_paths(source_dir) == []
    return True


def test_gmted_source_tiles_rejects_a_bbox():
    """GMTED's only download is one whole-globe archive; asking for a
    sub-region is a mistake worth reporting, not silently ignoring."""
    a = get_adapter("gmted2010-30")
    try:
        a.source_tiles((-125.0, 24.0, -66.0, 50.0))
        assert False, "expected ValueError"
    except ValueError as e:
        assert "bbox" in str(e).lower()
    return True


def test_copernicus_registry_and_metadata():
    for name in ("copernicus-glo90", "copernicus-glo30"):
        a = get_adapter(name)
        assert isinstance(a, CopernicusGloAdapter)
        assert a is DATASETS[name]
        assert a.name == name
        assert a.native_vertical_datum == "EGM2008"  # client target; no conversion
        assert a.is_surface_model is True
        assert a.native_nodata is None
        assert a.vertical_precision_m == 1.0 / 256  # float32 source, B populated
        assert a.verify_download_size is False
        assert "COPERNICUS by the European Union and ESA" in a.attribution
    return True


def test_copernicus_resolutions_differ_only_in_resolution():
    glo90, glo30 = get_adapter("copernicus-glo90"), get_adapter("copernicus-glo30")
    assert glo90.native_post_spacing_m == glo30.native_post_spacing_m * 3.0
    assert "90m" in glo90.bucket_url and "30m" in glo30.bucket_url
    assert "WorldDEM-90" in glo90.attribution and "WorldDEM-30" in glo30.attribution
    for field in ("source_version", "native_vertical_datum", "is_surface_model",
                  "native_nodata", "vertical_precision_m", "verify_download_size"):
        assert getattr(glo90, field) == getattr(glo30, field)
    return True


def test_copernicus_tile_name_parsing():
    assert _parse_copernicus_cell("Copernicus_DSM_COG_30_N39_00_W120_00_DEM") == (39, -120)
    assert _parse_copernicus_cell("Copernicus_DSM_COG_10_S90_00_W178_00_DEM") == (-90, -178)
    assert _parse_copernicus_cell("Copernicus_DSM_COG_30_N00_00_E006_00_DEM") == (0, 6)
    assert _parse_copernicus_cell("tileList.txt") is None
    return True


def test_copernicus_cell_bbox_overlap():
    conus = (-125.0, 24.0, -66.0, 50.0)
    assert _cell_overlaps_bbox((39, -120), conus)     # inside
    assert not _cell_overlaps_bbox((39, -130), conus)  # west of it
    assert not _cell_overlaps_bbox((60, -120), conus)  # north of it
    assert _cell_overlaps_bbox((49, -67), conus)       # NE corner cell
    # A bbox with west > east wraps the antimeridian.
    wrap = (170.0, -20.0, -170.0, 20.0)
    assert _cell_overlaps_bbox((0, 179), wrap)
    assert _cell_overlaps_bbox((0, -180), wrap)
    assert not _cell_overlaps_bbox((0, 0), wrap)
    return True


def test_copernicus_source_tiles_filters_the_tile_list():
    a = get_adapter("copernicus-glo90")
    tile_list = (
        "Copernicus_DSM_COG_30_N39_00_W120_00_DEM\n"   # in CONUS
        "Copernicus_DSM_COG_30_N00_00_E006_00_DEM\n"    # Gulf of Guinea, out
        "Copernicus_DSM_COG_30_N45_00_W110_00_DEM\n"    # in CONUS
        "not_a_tile_name\n"
    )
    original = CopernicusGloAdapter._fetch_tile_list
    CopernicusGloAdapter._fetch_tile_list = lambda self: tile_list.split()
    try:
        # Each kept cell yields a DEM tile plus its optional WBM companion.
        all_tiles = a.source_tiles()
        assert len(all_tiles) == 6  # 3 cells (junk line dropped) x (DEM + WBM)
        conus = a.source_tiles((-125.0, 24.0, -66.0, 50.0))
        assert [t.local_name for t in conus] == [
            "Copernicus_DSM_COG_30_N39_00_W120_00_DEM.tif",
            "Copernicus_DSM_COG_30_N39_00_W120_00_WBM.tif",
            "Copernicus_DSM_COG_30_N45_00_W110_00_DEM.tif",
            "Copernicus_DSM_COG_30_N45_00_W110_00_WBM.tif",
        ]
        assert [t.optional for t in conus] == [False, True, False, True]
        assert conus[0].url == (
            "https://copernicus-dem-90m.s3.amazonaws.com/"
            "Copernicus_DSM_COG_30_N39_00_W120_00_DEM/"
            "Copernicus_DSM_COG_30_N39_00_W120_00_DEM.tif"
        )
        assert conus[1].url == (
            "https://copernicus-dem-90m.s3.amazonaws.com/"
            "Copernicus_DSM_COG_30_N39_00_W120_00_DEM/AUXFILES/"
            "Copernicus_DSM_COG_30_N39_00_W120_00_WBM.tif"
        )
    finally:
        CopernicusGloAdapter._fetch_tile_list = original
    return True


def test_copernicus_source_paths_globs_downloaded_cogs():
    a = get_adapter("copernicus-glo30")
    with tempfile.TemporaryDirectory() as source_dir:
        for name in ("Copernicus_DSM_COG_10_N39_00_W120_00_DEM.tif",
                     "Copernicus_DSM_COG_10_N45_00_W110_00_DEM.tif",
                     "tileList.txt", "notes.txt"):
            open(os.path.join(source_dir, name), "w").close()
        assert a.source_paths(source_dir) == [
            os.path.join(source_dir, "Copernicus_DSM_COG_10_N39_00_W120_00_DEM.tif"),
            os.path.join(source_dir, "Copernicus_DSM_COG_10_N45_00_W110_00_DEM.tif"),
        ]
    assert a.source_paths("/nonexistent") == []
    return True


def test_copernicus_source_paths_ignores_the_other_resolution():
    """GLO-90 COGs are named ..._COG_30_... and GLO-30 ..._COG_10_...;
    a directory holding both must not cross-contaminate a build."""
    glo90, glo30 = get_adapter("copernicus-glo90"), get_adapter("copernicus-glo30")
    with tempfile.TemporaryDirectory() as source_dir:
        glo90_tile = "Copernicus_DSM_COG_30_N39_00_W120_00_DEM.tif"
        glo30_tile = "Copernicus_DSM_COG_10_N39_00_W120_00_DEM.tif"
        for name in (glo90_tile, glo30_tile):
            open(os.path.join(source_dir, name), "w").close()
        assert glo90.source_paths(source_dir) == [os.path.join(source_dir, glo90_tile)]
        assert glo30.source_paths(source_dir) == [os.path.join(source_dir, glo30_tile)]
    return True


def test_copernicus_water_source_paths():
    glo90 = get_adapter("copernicus-glo90")
    with tempfile.TemporaryDirectory() as source_dir:
        for name in ("Copernicus_DSM_COG_30_N39_00_W120_00_DEM.tif",
                     "Copernicus_DSM_COG_30_N39_00_W120_00_WBM.tif",
                     "Copernicus_DSM_COG_30_N45_00_W110_00_WBM.tif",
                     "Copernicus_DSM_COG_10_N39_00_W120_00_WBM.tif"):  # wrong resolution
            open(os.path.join(source_dir, name), "w").close()
        assert glo90.water_source_paths(source_dir) == [
            os.path.join(source_dir, "Copernicus_DSM_COG_30_N39_00_W120_00_WBM.tif"),
            os.path.join(source_dir, "Copernicus_DSM_COG_30_N45_00_W110_00_WBM.tif"),
        ]
    return True


TESTS = [
    test_registry_lookup,
    test_resolution_variants_differ_only_in_resolution,
    test_registry_unknown_dataset,
    test_base_class_is_unusable_directly,
    test_declared_metadata,
    test_source_tiles_returns_the_global_archive_plus_gshhg,
    test_gmted_gshhg_zip_path,
    test_source_paths_extracts_the_zip_once,
    test_source_paths_replaces_an_incomplete_extraction,
    test_source_paths_empty_when_not_yet_downloaded,
    test_gmted_source_tiles_rejects_a_bbox,
    test_copernicus_registry_and_metadata,
    test_copernicus_resolutions_differ_only_in_resolution,
    test_copernicus_tile_name_parsing,
    test_copernicus_cell_bbox_overlap,
    test_copernicus_source_tiles_filters_the_tile_list,
    test_copernicus_source_paths_globs_downloaded_cogs,
    test_copernicus_source_paths_ignores_the_other_resolution,
    test_copernicus_water_source_paths,
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
