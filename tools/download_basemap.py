#!/usr/bin/env python3
"""Download the Natural Earth basemap source for build_basemap.py.

Usage:
    python3 tools/download_basemap.py [output_dir]
"""

import argparse
import os
import shutil
import urllib.request

# The full Natural Earth vector GeoPackage (every theme, every scale), ~426 MB.
# build_basemap.py reads the handful of layers it needs straight out of this
# archive; there is no smaller published package.
SOURCE_URL = "https://naciscdn.org/naturalearth/packages/natural_earth_vector.gpkg.zip"
LOCAL_NAME = "natural_earth_vector.gpkg.zip"


def _remote_content_length(url):
    """The server's declared size for `url` in bytes via HEAD, or None."""
    request = urllib.request.Request(url, method="HEAD")
    with urllib.request.urlopen(request) as response:
        length = response.headers.get("Content-Length")
        return int(length) if length is not None else None


def download(url, dest_path, force=False):
    """Download `url` to `dest_path` atomically via a .part file.

    Skips the download when `dest_path` already exists at the server's
    declared size, unless `force`. Returns "downloaded" or "skipped".
    """
    if not force and os.path.exists(dest_path):
        remote_size = _remote_content_length(url)
        if remote_size is None or os.path.getsize(dest_path) == remote_size:
            return "skipped"

    tmp_path = dest_path + ".part"
    with urllib.request.urlopen(url) as response:
        content_length = response.headers.get("Content-Length")
        with open(tmp_path, "wb") as f:
            shutil.copyfileobj(response, f)

    downloaded_size = os.path.getsize(tmp_path)
    if content_length is not None and downloaded_size != int(content_length):
        os.remove(tmp_path)
        raise ValueError(
            f"{LOCAL_NAME}: downloaded {downloaded_size} bytes, "
            f"expected {content_length} from Content-Length"
        )
    os.replace(tmp_path, dest_path)
    return "downloaded"


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Download the Natural Earth basemap source for build_basemap.py."
    )
    parser.add_argument("--force", action="store_true",
                        help="Re-download even if a correctly-sized file already exists")
    parser.add_argument("output_dir", nargs="?", default="mapdata",
                        help="Directory to save the archive into (default: mapdata/)")
    args = parser.parse_args(argv)

    os.makedirs(args.output_dir, exist_ok=True)
    dest_path = os.path.join(args.output_dir, LOCAL_NAME)

    print(f"Downloading Natural Earth basemap source to {args.output_dir}/")
    result = download(SOURCE_URL, dest_path, force=args.force)
    size_mb = os.path.getsize(dest_path) / (1024 * 1024)
    if result == "skipped":
        print(f"  {LOCAL_NAME}: already downloaded ({size_mb:.1f} MB), skipping")
    else:
        print(f"  Downloaded {LOCAL_NAME} ({size_mb:.1f} MB)")

    print("\nTo build:")
    print(f"  python3 tools/build_basemap.py {dest_path} basemap/")


if __name__ == "__main__":
    main()
