#!/usr/bin/env python3
"""Tests for terrain_manifest.py."""

import json
import os
import sys
import tempfile

import terrain_common
from terrain_datasets import DatasetAdapter
from terrain_manifest import (
    MANIFEST_FILENAME,
    build_manifest,
    read_manifest,
    write_manifest,
)


class _FakeAdapter(DatasetAdapter):
    name = "faketest_terrain_adapter"
    display_name = "Fake Test Dataset"
    source_version = "2026-01-01"
    attribution = "Fake Test Data Consortium. Public domain."
    native_vertical_datum = "EGM96"  # deliberately NOT the client's target datum
    native_nodata = -9999.0
    is_surface_model = True
    native_post_spacing_m = 100.0
    vertical_precision_m = terrain_common.VERTICAL_PRECISION_WHOLE_METRE


def test_build_manifest_fields():
    adapter = _FakeAdapter()
    manifest = build_manifest(adapter, 3, 6, (-10.0, 20.0, 10.0, 40.0))

    assert manifest["dataset"] == "faketest_terrain_adapter"
    assert manifest["dataset_display_name"] == "Fake Test Dataset"
    assert manifest["source_version"] == "2026-01-01"
    assert manifest["attribution"] == adapter.attribution
    assert manifest["is_surface_model"] is True
    assert manifest["vertical_precision_m"] == terrain_common.VERTICAL_PRECISION_WHOLE_METRE
    assert manifest["tile_pixels"] == terrain_common.TILE_PIXELS
    assert manifest["skirt_pixels"] == terrain_common.SKIRT_PIXELS
    assert manifest["min_zoom"] == 3
    assert manifest["max_zoom"] == 6
    assert manifest["bbox"] == [-10.0, 20.0, 10.0, 40.0]
    assert isinstance(manifest["last_updated"], str) and manifest["last_updated"]
    return True


def test_vertical_datum_is_always_the_target_not_the_source():
    """The manifest records what's actually IN the file (the client's
    single datum) -- never the adapter's pre-conversion native one,
    even though this fake adapter deliberately declares a different
    native_vertical_datum above."""
    adapter = _FakeAdapter()
    manifest = build_manifest(adapter, 0, 0, (0, 0, 1, 1))
    assert manifest["vertical_datum"] == "EGM2008"
    assert manifest["vertical_datum"] != adapter.native_vertical_datum
    return True


def test_write_manifest_is_valid_json_and_leaves_no_temp_file():
    adapter = _FakeAdapter()
    with tempfile.TemporaryDirectory() as output_dir:
        written = write_manifest(adapter, output_dir, 2, 5, (-1.0, -1.0, 1.0, 1.0))

        path = os.path.join(output_dir, MANIFEST_FILENAME)
        assert os.path.exists(path)
        assert not os.path.exists(path + ".tmp")

        with open(path) as f:
            on_disk = json.load(f)
        assert on_disk == written
        assert on_disk["min_zoom"] == 2 and on_disk["max_zoom"] == 5
    return True


def test_read_manifest_roundtrips_and_returns_none_when_absent():
    adapter = _FakeAdapter()
    with tempfile.TemporaryDirectory() as output_dir:
        assert read_manifest(output_dir) is None
        written = write_manifest(adapter, output_dir, 2, 5, (-1.0, -1.0, 1.0, 1.0))
        assert read_manifest(output_dir) == written
    return True


TESTS = [
    test_build_manifest_fields,
    test_vertical_datum_is_always_the_target_not_the_source,
    test_write_manifest_is_valid_json_and_leaves_no_temp_file,
    test_read_manifest_roundtrips_and_returns_none_when_absent,
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
