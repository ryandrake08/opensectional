#!/usr/bin/env python3
"""Tests for the Terrarium elevation codec in terrain_common.py."""

import os
import sys
import tempfile

import numpy as np

from terrain_common import (
    FULL_TILE_PIXELS,
    MAX_ELEVATION_M,
    MIN_ELEVATION_M,
    NODATA_ELEVATION_M,
    SKIRT_PIXELS,
    TILE_PIXELS,
    VERTICAL_PRECISION_SUBMETRE,
    VERTICAL_PRECISION_WHOLE_METRE,
    WATER_LAKE,
    WATER_OCEAN,
    WATER_RIVER,
    decode_elevation,
    decode_elevation_array,
    decode_water_array,
    encode_elevation,
    encode_elevation_array,
    encode_water_array,
    extract_interior,
    interior_slice,
    load_tile_png,
    save_tile_png,
)


def test_scalar_roundtrip():
    """A handful of representative values round-trip at both precisions.

    Boundary values are precision-specific: MIN/MAX_ELEVATION_M are the
    sub-metre format limits, one fixed-point step (1/256 m) off the
    sentinel -- rounding either to the nearest whole metre lands back
    on the sentinel itself, so whole-metre precision is exercised at
    its own boundary, one full metre in.
    """
    common_values = (0.0, 1.0, -1.0, 8849.0, -430.0, 100.0)
    boundaries = {
        VERTICAL_PRECISION_SUBMETRE: (MIN_ELEVATION_M, MAX_ELEVATION_M),
        VERTICAL_PRECISION_WHOLE_METRE: (NODATA_ELEVATION_M + 1.0, MAX_ELEVATION_M - 1.0),
    }
    for precision in (VERTICAL_PRECISION_WHOLE_METRE, VERTICAL_PRECISION_SUBMETRE):
        for elevation_m in common_values + boundaries[precision]:
            r, g, b = encode_elevation(elevation_m, precision)
            decoded = decode_elevation(r, g, b)
            expected = round(elevation_m / precision) * precision
            assert decoded == expected, (
                f"precision={precision} elevation_m={elevation_m} "
                f"decoded={decoded} expected={expected}"
            )
    return True


def test_nodata_sentinel():
    """None <-> (0, 0, 0) in both directions, at both precisions."""
    for precision in (VERTICAL_PRECISION_WHOLE_METRE, VERTICAL_PRECISION_SUBMETRE):
        assert encode_elevation(None, precision) == (0, 0, 0)
    assert decode_elevation(0, 0, 0) is None
    assert NODATA_ELEVATION_M == -32768.0
    return True


def test_reserved_sentinel_rejected():
    """Encoding a real elevation that quantizes to the sentinel raises."""
    for precision in (VERTICAL_PRECISION_WHOLE_METRE, VERTICAL_PRECISION_SUBMETRE):
        try:
            encode_elevation(NODATA_ELEVATION_M, precision)
            assert False, f"expected ValueError at precision={precision}"
        except ValueError:
            pass
    return True


def test_range_validation():
    """Values outside [MIN_ELEVATION_M, MAX_ELEVATION_M] are rejected."""
    for bad in (MIN_ELEVATION_M - 1.0, MAX_ELEVATION_M + 1.0):
        try:
            encode_elevation(bad)
            assert False, f"expected ValueError for elevation_m={bad}"
        except ValueError:
            pass
    return True


def test_invalid_precision_rejected():
    for bad in (0.5, 2.0, 1.0 / 128, "1"):
        for fn, args in (
            (encode_elevation, (0.0, bad)),
            (encode_elevation_array, (np.zeros((2, 2)), bad)),
        ):
            try:
                fn(*args)  # type: ignore  # deliberately passes invalid precision values
                assert False, f"expected ValueError for vertical_precision_m={bad!r}"
            except (ValueError, TypeError):
                pass
    return True


def test_array_roundtrip_submetre_full_range():
    """Every representable raw value round-trips exactly at sub-metre
    precision -- the strongest possible check of the format itself,
    covering the codec's entire domain rather than a sample of it."""
    raw = np.arange(1, 0x1000000, dtype=np.int64)  # excludes the reserved 0
    r = ((raw >> 16) & 0xFF).astype(np.uint8)
    g = ((raw >> 8) & 0xFF).astype(np.uint8)
    b = (raw & 0xFF).astype(np.uint8)
    rgb = np.stack([r, g, b], axis=-1)

    elevation = decode_elevation_array(rgb)
    assert not np.any(np.isnan(elevation))
    assert elevation.min() == MIN_ELEVATION_M
    assert elevation.max() == MAX_ELEVATION_M

    reencoded = encode_elevation_array(elevation, VERTICAL_PRECISION_SUBMETRE)
    assert np.array_equal(reencoded, rgb)
    return True


def test_array_roundtrip_whole_metre_full_range():
    """Every representable whole-metre value round-trips exactly."""
    metres = np.arange(-32767, 32768, dtype=np.float64)  # excludes the sentinel (-32768)
    rgb = encode_elevation_array(metres, VERTICAL_PRECISION_WHOLE_METRE)
    assert np.all(rgb[..., 2] == 0), "whole-metre encoding must always leave B = 0"

    decoded = decode_elevation_array(rgb)
    assert np.array_equal(decoded, metres)
    return True


def test_array_nodata_roundtrip():
    """NaN in, sentinel bytes out, NaN back."""
    elevation = np.array([[0.0, np.nan], [-430.0, 8849.0]])
    rgb = encode_elevation_array(elevation)
    assert tuple(rgb[0, 1]) == (0, 0, 0)
    decoded = decode_elevation_array(rgb)
    assert np.isnan(decoded[0, 1])
    assert np.array_equal(decoded[~np.isnan(decoded)], elevation[~np.isnan(elevation)])
    return True


def test_skirt_geometry():
    assert TILE_PIXELS == 256
    assert SKIRT_PIXELS == 2
    assert FULL_TILE_PIXELS == 260

    tile = np.full((FULL_TILE_PIXELS, FULL_TILE_PIXELS), -1, dtype=np.int32)
    interior = np.arange(TILE_PIXELS * TILE_PIXELS, dtype=np.int32).reshape(TILE_PIXELS, TILE_PIXELS)
    rows, cols = interior_slice()
    tile[rows, cols] = interior

    extracted = extract_interior(tile)
    assert np.array_equal(extracted, interior)

    # The skirt border (everything outside the interior slice) is untouched.
    assert np.all(tile[:SKIRT_PIXELS, :] == -1)
    assert np.all(tile[-SKIRT_PIXELS:, :] == -1)
    assert np.all(tile[:, :SKIRT_PIXELS] == -1)
    assert np.all(tile[:, -SKIRT_PIXELS:] == -1)
    return True


def test_png_roundtrip():
    """The codec survives an actual PNG file write/read, not just the
    in-memory array formulas."""
    rng = np.random.default_rng(1234)
    elevation = rng.uniform(-430.0, 8849.0, size=(FULL_TILE_PIXELS, FULL_TILE_PIXELS))
    elevation[0, 0] = np.nan  # a no-data corner, e.g. ocean
    rgb = encode_elevation_array(elevation, VERTICAL_PRECISION_SUBMETRE)

    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "tile.png")
        save_tile_png(path, rgb)
        loaded = load_tile_png(path)

    assert np.array_equal(loaded, rgb)
    decoded = decode_elevation_array(loaded)
    assert np.isnan(decoded[0, 0])
    close = np.abs(decoded[1:, 1:] - elevation[1:, 1:]) <= VERTICAL_PRECISION_SUBMETRE / 2
    assert np.all(close)
    return True


def test_water_array_roundtrip():
    rng = np.random.default_rng(7)
    classes = rng.integers(0, WATER_RIVER + 1, size=(FULL_TILE_PIXELS, FULL_TILE_PIXELS), dtype=np.uint8)
    rgb = encode_water_array(classes)
    assert rgb.shape == (FULL_TILE_PIXELS, FULL_TILE_PIXELS, 3)
    assert np.all(rgb[..., 1:] == 0)  # class rides in R only
    assert np.array_equal(decode_water_array(rgb), classes)
    return True


def test_water_png_roundtrip():
    classes = np.full((FULL_TILE_PIXELS, FULL_TILE_PIXELS), WATER_OCEAN, dtype=np.uint8)
    classes[10:20, 10:20] = WATER_LAKE
    classes[30, :] = WATER_RIVER
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "water.png")
        save_tile_png(path, encode_water_array(classes))
        loaded = load_tile_png(path)
    assert np.array_equal(decode_water_array(loaded), classes)
    return True


def test_water_array_rejects_out_of_range_class():
    try:
        encode_water_array(np.array([[0, 1], [2, 9]], dtype=np.uint8))
        assert False, "expected ValueError"
    except ValueError:
        pass
    return True


TESTS = [
    test_scalar_roundtrip,
    test_nodata_sentinel,
    test_reserved_sentinel_rejected,
    test_range_validation,
    test_invalid_precision_rejected,
    test_array_roundtrip_submetre_full_range,
    test_array_roundtrip_whole_metre_full_range,
    test_array_nodata_roundtrip,
    test_skirt_geometry,
    test_png_roundtrip,
    test_water_array_roundtrip,
    test_water_png_roundtrip,
    test_water_array_rejects_out_of_range_class,
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
