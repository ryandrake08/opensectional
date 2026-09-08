#!/usr/bin/env python3
"""Download source rasters for terrain builds.

Usage:
    python3 tools/download_terrain.py --dataset gmted2010-30 output_dir
    python3 tools/download_terrain.py --dataset copernicus-glo90 \\
        --bbox=-125,24,-66,50 output_dir

A negative western longitude makes --bbox look like a flag to argparse,
so the value must be attached with '=' as shown.
"""

import argparse
import concurrent.futures
import itertools
import os
import shutil
import urllib.request

import http_retry
from terrain_datasets import DATASETS, get_adapter


def positive_int(text):
    """argparse type for a strictly-positive integer."""
    value = int(text)
    if value < 1:
        raise argparse.ArgumentTypeError(f"must be >= 1, got {value}")
    return value


def parse_bbox(text):
    """Parse a "west,south,east,north" degree string into a float tuple.

    West may exceed east to wrap the antimeridian; latitudes may not be
    inverted or leave [-90, 90], and longitudes may not leave
    [-180, 180]."""
    parts = text.split(",")
    if len(parts) != 4:
        raise ValueError(f"--bbox must be 'west,south,east,north', got {text!r}")
    try:
        west, south, east, north = (float(p) for p in parts)
    except ValueError:
        raise ValueError(f"--bbox values must be numbers, got {text!r}") from None
    if not (-90.0 <= south < north <= 90.0):
        raise ValueError(f"--bbox latitudes must satisfy -90 <= south < north <= 90, got {south},{north}")
    if not (-180.0 <= west <= 180.0 and -180.0 <= east <= 180.0):
        raise ValueError(f"--bbox longitudes must be within [-180, 180], got {west},{east}")
    return west, south, east, north


def _remote_content_length(url, label):
    """Return the server's declared size for `url` in bytes via HEAD,
    or None if the server doesn't report one."""
    def head():
        request = urllib.request.Request(url, method="HEAD")
        with urllib.request.urlopen(request, timeout=http_retry.TIMEOUT_S) as response:
            length = response.headers.get("Content-Length")
            return int(length) if length is not None else None
    return http_retry.retry(head, label=label)


def download_tile(tile, dest_path, force=False, verify_size=True):
    """Download one source tile atomically, returning its result.

    An already-present `dest_path` is skipped: when `verify_size`, only
    if it matches the server's declared Content-Length; otherwise on
    existence alone (for datasets of many immutable files where a HEAD
    per file would dominate a resumed run). A freshly downloaded file is
    always size-checked against the GET response's Content-Length.

    Transient network failures are retried with backoff; a truncated
    body raises and is not retried."""
    if not force and os.path.exists(dest_path):
        if not verify_size:
            return "skipped"
        remote_size = _remote_content_length(tile.url, tile.local_name)
        if remote_size is None or os.path.getsize(dest_path) == remote_size:
            return "skipped"

    tmp_path = dest_path + ".part"

    def get():
        with urllib.request.urlopen(tile.url, timeout=http_retry.TIMEOUT_S) as response:
            content_length = response.headers.get("Content-Length")
            with open(tmp_path, "wb") as f:
                shutil.copyfileobj(response, f)
        downloaded_size = os.path.getsize(tmp_path)
        if content_length is not None and downloaded_size != int(content_length):
            raise ValueError(
                f"{tile.local_name}: downloaded {downloaded_size} bytes, "
                f"expected {content_length} from Content-Length"
            )
        os.replace(tmp_path, dest_path)
        return "downloaded"

    try:
        return http_retry.retry(get, label=tile.local_name)
    except BaseException:
        if os.path.exists(tmp_path):
            os.remove(tmp_path)
        raise


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Download DEM source rasters for the terrain ingester."
    )
    parser.add_argument("--dataset", required=True, choices=sorted(DATASETS), metavar="NAME",
                        help=f"Dataset to download. Choices: {', '.join(sorted(DATASETS))}.")
    parser.add_argument("--force", action="store_true",
                        help="Re-download even if a correctly-sized file already exists")
    parser.add_argument("--bbox", metavar="W,S,E,N", type=parse_bbox, default=None,
                        help="Only fetch tiles overlapping this west,south,east,north degree "
                             "box (per-tile datasets only; default is the whole dataset). "
                             "Attach the value with '=' so a negative west longitude is not "
                             "read as a flag: --bbox=-125,24,-66,50")
    parser.add_argument("--jobs", type=positive_int, default=4, metavar="N",
                        help="Parallel downloads (default: 4). Values past ~6-8 trip "
                             "AWS's anonymous S3 rate limiting; the retry backoff recovers "
                             "but slowly.")
    parser.add_argument("source_dir", help="Directory to save downloaded source rasters into")
    args = parser.parse_args(argv)

    adapter = get_adapter(args.dataset)
    tiles = adapter.source_tiles(args.bbox)
    if not tiles:
        where = " in the requested bbox" if args.bbox is not None else ""
        raise ValueError(f"{args.dataset} has no source tiles to download{where}")

    os.makedirs(args.source_dir, exist_ok=True)
    print(f"Downloading {len(tiles)} {adapter.display_name} tile(s) to {args.source_dir}/ "
          f"with {args.jobs} job(s)")

    def fetch(tile):
        dest_path = os.path.join(args.source_dir, tile.local_name)
        result = download_tile(tile, dest_path, force=args.force,
                               verify_size=adapter.verify_download_size)
        return tile, dest_path, result

    downloaded = skipped = 0
    pending_tiles = iter(tiles)
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        # Keep at most 2*jobs futures alive: a whole-world job is
        # hundreds of thousands of tiles, and a small backlog means a
        # mid-run failure cancels most of the queued work instead of
        # draining it.
        pending = {pool.submit(fetch, t) for t in itertools.islice(pending_tiles, 2 * args.jobs)}
        try:
            while pending:
                done, pending = concurrent.futures.wait(
                    pending, return_when=concurrent.futures.FIRST_COMPLETED)
                for future in done:
                    tile, dest_path, result = future.result()
                    if result == "skipped":
                        skipped += 1
                    else:
                        size_mb = os.path.getsize(dest_path) / (1024 * 1024)
                        print(f"  Downloaded {tile.local_name} ({size_mb:.1f} MB)")
                        downloaded += 1
                pending |= {pool.submit(fetch, t)
                            for t in itertools.islice(pending_tiles, len(done))}
        except BaseException:
            for future in pending:
                future.cancel()
            raise

    print(f"\n{downloaded} downloaded, {skipped} skipped, of {len(tiles)} total, in {args.source_dir}/")
    print("\nTo build:")
    print(f"  python3 tools/build_terrain.py --dataset {adapter.name} --zoom <min-max> "
          f"{args.source_dir} <output_dir>")


if __name__ == "__main__":
    main()
