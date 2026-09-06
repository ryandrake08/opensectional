"""Terrarium RGB elevation codec shared by the terrain ingesters.

Every terrain tile on disk is a z/x/y.png encoded with the Terrarium
convention:

    elevation_m = (R * 256 + G + B / 256) - 32768

R and G alone give 1 m steps; B extends that to the format's full
1/256 m (~3.9 mm) fixed-point precision. Whether a given tile's B
channel is populated is a per-dataset ingester policy (see
`vertical_precision_m` below), not a per-pixel choice, and is recorded
in that tile tree's manifest so the client can decode without knowing
which dataset produced it.

The all-zero pixel (0, 0, 0) is the reserved no-data sentinel. It
decodes to exactly -32768 m, more than 32 km below the deepest point
on land (the Dead Sea shore, -430 m), so it can never collide with a
real elevation.
"""

import numpy as np
from PIL import Image

# --- Tile geometry -------------------------------------------------------

TILE_PIXELS = 256
SKIRT_PIXELS = 2
FULL_TILE_PIXELS = TILE_PIXELS + 2 * SKIRT_PIXELS  # 260

# Every tile is rendered 2 px larger than its display size on each side.
# A hillshade needs each pixel's neighbours to compute slope; without a
# skirt, that neighbour lookup at a tile's own edge would have to reach
# into the adjacent tile, producing a seam wherever the two tiles were
# built independently. Baking the skirt into the stored tile removes
# the seam and removes any need to sample a second tile at render time.


def interior_slice():
    """The (row_slice, col_slice) of the 256x256 visible interior within
    a 260x260 tile array."""
    s = slice(SKIRT_PIXELS, SKIRT_PIXELS + TILE_PIXELS)
    return s, s


def extract_interior(tile):
    """Return the 256x256 interior of a 260x260 tile array (a view, not
    a copy)."""
    rows, cols = interior_slice()
    return tile[rows, cols]


# --- Elevation encoding ----------------------------------------------------

ELEVATION_BIAS_M = 32768.0

# The two vertical precisions a tile tree may declare. Whole-metre
# rounds to the nearest metre and always writes B = 0 -- a constant
# channel compresses to almost nothing, so this costs little even
# though it discards the format's finer steps. Sub-metre keeps the
# format's full fixed-point precision. See `manifest.json`'s
# `vertical_precision_m` field: this module never picks one on its
# own, an ingester declares it per dataset.
VERTICAL_PRECISION_WHOLE_METRE = 1.0
VERTICAL_PRECISION_SUBMETRE = 1.0 / 256

NODATA_ELEVATION_M = -ELEVATION_BIAS_M
MIN_ELEVATION_M = -ELEVATION_BIAS_M + VERTICAL_PRECISION_SUBMETRE
MAX_ELEVATION_M = ELEVATION_BIAS_M - VERTICAL_PRECISION_SUBMETRE


def _validate_precision(vertical_precision_m):
    if vertical_precision_m not in (VERTICAL_PRECISION_WHOLE_METRE, VERTICAL_PRECISION_SUBMETRE):
        raise ValueError(
            "vertical_precision_m must be VERTICAL_PRECISION_WHOLE_METRE "
            f"({VERTICAL_PRECISION_WHOLE_METRE}) or VERTICAL_PRECISION_SUBMETRE "
            f"({VERTICAL_PRECISION_SUBMETRE}), got {vertical_precision_m!r}"
        )


def _encode_raw(elevation_m, vertical_precision_m):
    """Shared quantization step for the scalar and array encoders.

    Returns the 24-bit integer `R*65536 + G*256 + B`. Raises ValueError
    if `elevation_m` quantizes to the reserved sentinel (0) or overflows
    the 24-bit range -- both only reachable within one precision step of
    MIN_ELEVATION_M / MAX_ELEVATION_M.
    """
    steps_per_metre = round(1.0 / vertical_precision_m)  # 1 or 256
    scale = 256 // steps_per_metre
    raw = round((elevation_m + ELEVATION_BIAS_M) * steps_per_metre) * scale
    if raw <= 0 or raw > 0xFFFFFF:
        raise ValueError(
            f"elevation_m={elevation_m} quantizes to the reserved no-data "
            f"sentinel or overflows at vertical_precision_m={vertical_precision_m}"
        )
    return raw


def encode_elevation(elevation_m, vertical_precision_m=VERTICAL_PRECISION_SUBMETRE):
    """Encode one elevation value in metres into a Terrarium (r, g, b) triple.

    `elevation_m=None` encodes the no-data sentinel. Raises ValueError if
    `elevation_m` is outside the representable range or `vertical_precision_m`
    is not one of the two declared constants.
    """
    _validate_precision(vertical_precision_m)
    if elevation_m is None:
        return (0, 0, 0)
    if not (MIN_ELEVATION_M <= elevation_m <= MAX_ELEVATION_M):
        raise ValueError(
            f"elevation_m={elevation_m} outside the representable range "
            f"[{MIN_ELEVATION_M}, {MAX_ELEVATION_M}]"
        )
    raw = _encode_raw(elevation_m, vertical_precision_m)
    return (raw >> 16) & 0xFF, (raw >> 8) & 0xFF, raw & 0xFF


def decode_elevation(r, g, b):
    """Decode a Terrarium (r, g, b) triple into an elevation in metres.

    Returns None for the no-data sentinel (0, 0, 0).
    """
    if r == 0 and g == 0 and b == 0:
        return None
    return (r * 256 + g + b / 256.0) - ELEVATION_BIAS_M


def encode_elevation_array(elevation_m, vertical_precision_m=VERTICAL_PRECISION_SUBMETRE):
    """Vectorized `encode_elevation` over an array of any shape.

    `elevation_m` is a float array; NaN marks no-data. Returns a
    `elevation_m.shape + (3,)` uint8 array. Raises ValueError under the
    same conditions as the scalar form, checked across the whole array.
    """
    _validate_precision(vertical_precision_m)
    elevation_m = np.asarray(elevation_m, dtype=np.float64)
    nodata = np.isnan(elevation_m)
    real = elevation_m[~nodata]
    if real.size and (real.min() < MIN_ELEVATION_M or real.max() > MAX_ELEVATION_M):
        raise ValueError(
            "elevation array has values outside the representable range "
            f"[{MIN_ELEVATION_M}, {MAX_ELEVATION_M}]"
        )

    steps_per_metre = round(1.0 / vertical_precision_m)
    scale = 256 // steps_per_metre
    raw = np.zeros(elevation_m.shape, dtype=np.int64)
    raw[~nodata] = np.round((real + ELEVATION_BIAS_M) * steps_per_metre).astype(np.int64) * scale
    if np.any((raw[~nodata] <= 0) | (raw[~nodata] > 0xFFFFFF)):
        raise ValueError(
            "one or more elevation values quantize to the reserved no-data "
            f"sentinel or overflow at vertical_precision_m={vertical_precision_m}"
        )

    out = np.zeros(elevation_m.shape + (3,), dtype=np.uint8)
    out[..., 0] = (raw >> 16) & 0xFF
    out[..., 1] = (raw >> 8) & 0xFF
    out[..., 2] = raw & 0xFF
    out[nodata] = 0
    return out


def decode_elevation_array(rgb):
    """Vectorized `decode_elevation` over an (..., 3) uint8 array.

    Returns a float64 array of shape `rgb.shape[:-1]`, with NaN marking
    the no-data sentinel.
    """
    rgb = np.asarray(rgb, dtype=np.uint8)
    r = rgb[..., 0].astype(np.float64)
    g = rgb[..., 1].astype(np.float64)
    b = rgb[..., 2].astype(np.float64)
    elevation = r * 256.0 + g + b / 256.0 - ELEVATION_BIAS_M
    nodata = (rgb[..., 0] == 0) & (rgb[..., 1] == 0) & (rgb[..., 2] == 0)
    elevation[nodata] = np.nan
    return elevation


# --- Tile file I/O -----------------------------------------------------

def save_tile_png(path, rgb):
    """Write a (rows, cols, 3) uint8 Terrarium-encoded array as an RGB PNG.

    No alpha channel: the format needs none, and an RGB image ensures
    SDL3_image's paletted/grayscale alpha-mask path (meant for text and
    UI atlases; see `sdl::surface`) never applies to terrain tiles.
    """
    rgb = np.asarray(rgb, dtype=np.uint8)
    Image.fromarray(rgb, mode="RGB").save(path)


def load_tile_png(path):
    """Read a Terrarium-encoded PNG back into a (rows, cols, 3) uint8 array."""
    with Image.open(path) as img:
        return np.array(img.convert("RGB"), dtype=np.uint8)
