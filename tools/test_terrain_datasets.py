#!/usr/bin/env python3
"""Tests for the dataset adapter registry in terrain_datasets.py."""

import os
import sys
import tempfile
import zipfile

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
    a = get_adapter("gmted2010")
    assert a.name == "gmted2010"
    assert a.source_version == "20101117"
    assert a.native_vertical_datum == "EGM96"
    assert a.is_surface_model is True
    assert 900.0 < a.native_post_spacing_m < 950.0  # ~928 m at 30 arcsec
    assert a.vertical_precision_m == 1.0
    assert "public domain" in a.attribution.lower()
    return True


def test_source_tiles_returns_the_single_global_archive():
    a = get_adapter("gmted2010")
    tiles = a.source_tiles()
    assert len(tiles) == 1
    assert tiles[0].local_name == a.GRID_ZIP_NAME
    assert tiles[0].url.endswith(a.GRID_ZIP_NAME)
    return True


def test_source_paths_extracts_the_zip_once():
    """The ArcGrid entries inside the real archive are DEFLATE-compressed,
    which makes GDAL's /vsizip/ virtual filesystem far too slow for
    windowed reads (measured ~500x); source_paths extracts to a plain
    directory instead, and only does so once."""
    a = get_adapter("gmted2010")
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
    a = get_adapter("gmted2010")
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
    a = get_adapter("gmted2010")
    with tempfile.TemporaryDirectory() as source_dir:
        assert a.source_paths(source_dir) == []
    return True


TESTS = [
    test_registry_lookup,
    test_registry_unknown_dataset,
    test_base_class_is_unusable_directly,
    test_declared_metadata,
    test_source_tiles_returns_the_single_global_archive,
    test_source_paths_extracts_the_zip_once,
    test_source_paths_replaces_an_incomplete_extraction,
    test_source_paths_empty_when_not_yet_downloaded,
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
