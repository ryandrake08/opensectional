"""Build and write terrain manifest.json files."""

import datetime
import json
import os

import terrain_common
import terrain_datum

MANIFEST_FILENAME = "manifest.json"


def build_manifest(adapter, min_zoom, max_zoom, bbox):
    """Build a manifest for a terrain tile tree."""
    return {
        "dataset": adapter.name,
        "dataset_display_name": adapter.display_name,
        "source_version": adapter.source_version,
        "attribution": adapter.attribution,
        "is_surface_model": adapter.is_surface_model,
        "vertical_datum": terrain_datum.TARGET_DATUM,
        "vertical_precision_m": adapter.vertical_precision_m,
        "tile_pixels": terrain_common.TILE_PIXELS,
        "skirt_pixels": terrain_common.SKIRT_PIXELS,
        "min_zoom": min_zoom,
        "max_zoom": max_zoom,
        "bbox": list(bbox),
        "last_updated": datetime.datetime.now(datetime.UTC).replace(microsecond=0).isoformat(),
    }


def read_manifest(output_dir):
    """The manifest already in `output_dir`, or None if there isn't one."""
    path = os.path.join(output_dir, MANIFEST_FILENAME)
    if not os.path.exists(path):
        return None
    with open(path) as f:
        return json.load(f)


def write_manifest(adapter, output_dir, min_zoom, max_zoom, bbox):
    """Compute and atomically write manifest.json into output_dir."""
    manifest = build_manifest(adapter, min_zoom, max_zoom, bbox)
    path = os.path.join(output_dir, MANIFEST_FILENAME)
    tmp_path = path + ".tmp"
    with open(tmp_path, "w") as f:
        json.dump(manifest, f, indent=2, sort_keys=True)
        f.write("\n")
    os.replace(tmp_path, path)
    return manifest
