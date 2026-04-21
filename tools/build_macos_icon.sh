#!/usr/bin/env bash
# build_macos_icon.sh — Convert a square PNG into a macOS .icns app icon.
#
# Pipeline:
#   1. Shape: scale artwork to 830×830, center on a 1024×1024 transparent
#      canvas, and apply Apple's squircle mask (see spec below).
#   2. Slice: generate the 10 standard iconset sizes via `sips`.
#   3. Pack:  bundle the iconset into a .icns via `iconutil`.
#
# Shape spec:
#   Canvas  : 1024×1024, transparent outside the squircle
#   Squircle: 830×830, centered (97px transparent padding on all sides)
#   Corners : r=186px, continuous-curvature cubic bézier (k = r × 0.552)
#   Artwork : input scaled down by ~0.8105× (1024 → 830) before masking
#
# Usage:
#   ./build_macos_icon.sh <input.png> <output.icns>
#
# Requires:
#   - ImageMagick 6 or 7 (selected via $MAGICK, or auto-detected from PATH)
#   - sips, iconutil (macOS system tools)
#   - bc (float math for the squircle path)

set -euo pipefail

# ── Args ────────────────────────────────────────────────────────────────────
INPUT="${1:-}"
OUTPUT="${2:-}"

if [[ -z "$INPUT" || -z "$OUTPUT" ]]; then
    echo "Usage: $(basename "$0") <input.png> <output.icns>" >&2
    exit 1
fi

if [[ ! -f "$INPUT" ]]; then
    echo "Error: input file not found: $INPUT" >&2
    exit 1
fi

# ── Tool locations ──────────────────────────────────────────────────────────
# Prefer the caller-supplied $MAGICK (CMake passes the path it detected).
# Fallback: try `magick` (IM v7) then `convert` (IM v6 / legacy shim).
if [[ -z "${MAGICK:-}" ]]; then
    if   command -v magick  >/dev/null 2>&1; then MAGICK="magick"
    elif command -v convert >/dev/null 2>&1; then MAGICK="convert"
    else
        echo "Error: ImageMagick not found (need 'magick' or 'convert' in PATH)" >&2
        exit 1
    fi
fi

for tool in sips iconutil; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "Error: '$tool' not found — this script requires macOS." >&2
        exit 1
    fi
done

# ── Parameters ──────────────────────────────────────────────────────────────
CANVAS=1024   # output canvas size (px)
SQ=830        # squircle size (px)
PAD=97        # transparent border on each side  ((1024-830)/2)
R=186         # corner radius (px)
K=102.672     # bézier handle length  (R × 0.552)

# Squircle bounding box within the 1024×1024 canvas
X0=$PAD
X1=$(( PAD + SQ ))   # 927
Y0=$PAD
Y1=$(( PAD + SQ ))   # 927

# ── Helper: floating-point arithmetic via bc ────────────────────────────────
p() { printf "%.3f" "$(echo "$1" | bc -l)"; }

# ── Build squircle SVG path ─────────────────────────────────────────────────
#
# The path uses a continuous-curvature (smooth) corner approximation.
# Each corner is a single cubic bézier; control points are placed at
# distance k from the tangent points along each edge, which produces the
# same smooth curvature transition that Apple uses for macOS/iOS icons.
#
#   Top edge → top-right corner → right edge → bottom-right corner →
#   bottom edge → bottom-left corner → left edge → top-left corner → close
#
PATH_D="\
M $(p "$X0+$R"),${Y0} \
L $(p "$X1-$R"),${Y0} \
C $(p "$X1-$R+$K"),${Y0} ${X1},$(p "$Y0+$R-$K") ${X1},$(p "$Y0+$R") \
L ${X1},$(p "$Y1-$R") \
C ${X1},$(p "$Y1-$R+$K") $(p "$X1-$R+$K"),${Y1} $(p "$X1-$R"),${Y1} \
L $(p "$X0+$R"),${Y1} \
C $(p "$X0+$R-$K"),${Y1} ${X0},$(p "$Y1-$R+$K") ${X0},$(p "$Y1-$R") \
L ${X0},$(p "$Y0+$R") \
C ${X0},$(p "$Y0+$R-$K") $(p "$X0+$R-$K"),${Y0} $(p "$X0+$R"),${Y0} Z"

# ── Scratch workspace (auto-cleanup on exit) ────────────────────────────────
WORK=$(mktemp -d /tmp/macos_icon.XXXXXX)
trap 'rm -rf "$WORK"' EXIT

PLACED="$WORK/placed.png"
MASK="$WORK/mask.png"
SHAPED="$WORK/shaped.png"
ICONSET="$WORK/icon.iconset"
mkdir -p "$ICONSET"

# ── Step 1a: Scale artwork to 830×830, center on transparent 1024×1024 ──────
"$MAGICK" -size ${CANVAS}x${CANVAS} xc:none \
    \( "$INPUT" -resize ${SQ}x${SQ} \) \
    -geometry +${PAD}+${PAD} -composite \
    "$PLACED"

# ── Step 1b: Rasterize squircle as a white-on-black alpha mask ──────────────
"$MAGICK" -size ${CANVAS}x${CANVAS} xc:black \
    -fill white -draw "path '$PATH_D'" \
    "$MASK"

# ── Step 1c: Apply mask — white pixels opaque, black transparent ────────────
"$MAGICK" "$PLACED" "$MASK" \
    -alpha off -compose CopyOpacity -composite \
    "$SHAPED"

# ── Step 2: Slice iconset ───────────────────────────────────────────────────
sips -z 16   16   "$SHAPED" --out "$ICONSET/icon_16x16.png"       > /dev/null
sips -z 32   32   "$SHAPED" --out "$ICONSET/icon_16x16@2x.png"    > /dev/null
sips -z 32   32   "$SHAPED" --out "$ICONSET/icon_32x32.png"       > /dev/null
sips -z 64   64   "$SHAPED" --out "$ICONSET/icon_32x32@2x.png"    > /dev/null
sips -z 128  128  "$SHAPED" --out "$ICONSET/icon_128x128.png"     > /dev/null
sips -z 256  256  "$SHAPED" --out "$ICONSET/icon_128x128@2x.png"  > /dev/null
sips -z 256  256  "$SHAPED" --out "$ICONSET/icon_256x256.png"     > /dev/null
sips -z 512  512  "$SHAPED" --out "$ICONSET/icon_256x256@2x.png"  > /dev/null
sips -z 512  512  "$SHAPED" --out "$ICONSET/icon_512x512.png"     > /dev/null
sips -z 1024 1024 "$SHAPED" --out "$ICONSET/icon_512x512@2x.png"  > /dev/null

# ── Step 3: Pack into .icns ─────────────────────────────────────────────────
iconutil -c icns -o "$OUTPUT" "$ICONSET"

echo "Done: $OUTPUT"
