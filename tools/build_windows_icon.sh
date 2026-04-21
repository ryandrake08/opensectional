#!/usr/bin/env bash
# build_windows_icon.sh — Convert a square PNG into a Windows .ico app icon.
#
# Pipeline:
#   1. Mask: apply a full-canvas circular alpha mask (no resize, no padding).
#   2. Pack: emit a .ico with the standard Windows sizes embedded.
#
# Mask spec:
#   Canvas : 1024×1024 (same as input)
#   Circle : centered, diameter 1024 (edge-to-edge)
#
# Usage:
#   ./build_windows_icon.sh <input.png> <output.ico>
#
# Requires:
#   - ImageMagick 6 or 7 (selected via $MAGICK, or auto-detected from PATH)

set -euo pipefail

# ── Args ────────────────────────────────────────────────────────────────────
INPUT="${1:-}"
OUTPUT="${2:-}"

if [[ -z "$INPUT" || -z "$OUTPUT" ]]; then
    echo "Usage: $(basename "$0") <input.png> <output.ico>" >&2
    exit 1
fi

if [[ ! -f "$INPUT" ]]; then
    echo "Error: input file not found: $INPUT" >&2
    exit 1
fi

# ── Tool location ───────────────────────────────────────────────────────────
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

# ── Parameters ──────────────────────────────────────────────────────────────
CANVAS=1024
C=$(( CANVAS / 2 ))   # center coord

# ── Scratch workspace (auto-cleanup on exit) ────────────────────────────────
WORK=$(mktemp -d /tmp/windows_icon.XXXXXX)
trap 'rm -rf "$WORK"' EXIT

MASK="$WORK/mask.png"
SHAPED="$WORK/shaped.png"

# ── Step 1a: Rasterize circle as a white-on-black alpha mask ────────────────
"$MAGICK" -size ${CANVAS}x${CANVAS} xc:black \
    -fill white -draw "circle ${C},${C} ${C},0" \
    "$MASK"

# ── Step 1b: Apply mask — white pixels opaque, black transparent ────────────
"$MAGICK" "$INPUT" "$MASK" \
    -alpha off -compose CopyOpacity -composite \
    "$SHAPED"

# ── Step 2: Pack into .ico with standard Windows sizes ──────────────────────
"$MAGICK" "$SHAPED" \
    -define icon:auto-resize=16,32,48,64,128,256 \
    "$OUTPUT"

echo "Done: $OUTPUT"
