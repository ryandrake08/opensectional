#!/usr/bin/env python3
"""Download source rasters for terrain builds.

Usage:
    python3 tools/download_terrain.py --dataset gmted2010 terrain_source/gmted2010
"""

import argparse
import os
import shutil
import urllib.request

from terrain_datasets import DATASETS, get_adapter


def _remote_content_length(url):
    """Return the server's declared size for `url` in bytes via HEAD,
    or None if the server doesn't report one."""
    request = urllib.request.Request(url, method="HEAD")
    with urllib.request.urlopen(request) as response:
        length = response.headers.get("Content-Length")
        return int(length) if length is not None else None


def download_tile(tile, dest_path, force=False):
    """Download one source tile atomically, returning its result."""
    if not force and os.path.exists(dest_path):
        remote_size = _remote_content_length(tile.url)
        if remote_size is None or os.path.getsize(dest_path) == remote_size:
            return "skipped"

    tmp_path = dest_path + ".part"
    with urllib.request.urlopen(tile.url) as response:
        content_length = response.headers.get("Content-Length")
        with open(tmp_path, "wb") as f:
            shutil.copyfileobj(response, f)

    downloaded_size = os.path.getsize(tmp_path)
    if content_length is not None and downloaded_size != int(content_length):
        os.remove(tmp_path)
        raise ValueError(
            f"{tile.local_name}: downloaded {downloaded_size} bytes, "
            f"expected {content_length} from Content-Length"
        )
    os.replace(tmp_path, dest_path)
    return "downloaded"


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Download DEM source rasters for the terrain ingester."
    )
    parser.add_argument("--dataset", required=True, choices=sorted(DATASETS), metavar="NAME",
                        help=f"Dataset to download. Choices: {', '.join(sorted(DATASETS))}.")
    parser.add_argument("--force", action="store_true",
                        help="Re-download even if a correctly-sized file already exists")
    parser.add_argument("source_dir", help="Directory to save downloaded source rasters into")
    args = parser.parse_args(argv)

    adapter = get_adapter(args.dataset)
    tiles = adapter.source_tiles()
    if not tiles:
        raise ValueError(f"{args.dataset} declares no source tiles to download")

    os.makedirs(args.source_dir, exist_ok=True)
    print(f"Downloading {len(tiles)} {adapter.display_name} tile(s) to {args.source_dir}/")

    downloaded = skipped = 0
    for tile in tiles:
        dest_path = os.path.join(args.source_dir, tile.local_name)
        result = download_tile(tile, dest_path, force=args.force)
        if result == "skipped":
            print(f"  {tile.local_name}: already downloaded, skipping")
            skipped += 1
        else:
            size_mb = os.path.getsize(dest_path) / (1024 * 1024)
            print(f"  Downloaded {tile.local_name} ({size_mb:.1f} MB)")
            downloaded += 1

    print(f"\n{downloaded} downloaded, {skipped} skipped, of {len(tiles)} total, in {args.source_dir}/")
    print("\nTo build:")
    print(f"  python3 tools/build_terrain.py --dataset {adapter.name} {args.source_dir} <output_dir>")


if __name__ == "__main__":
    main()
