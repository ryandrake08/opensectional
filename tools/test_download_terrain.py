#!/usr/bin/env python3
"""Tests for terrain downloads using a local HTTP server."""

import contextlib
import functools
import http.server
import os
import sys
import tempfile
import threading
import urllib.error

from download_terrain import download_tile
from download_terrain import main as run_download_terrain
from terrain_datasets import Gmted2010Adapter, SourceTile, get_adapter


class _QuietHandler(http.server.SimpleHTTPRequestHandler):
    def log_message(self, format, *args):
        pass


@contextlib.contextmanager
def file_server(directory):
    """Serve `directory` over HTTP on a free local port."""
    handler = functools.partial(_QuietHandler, directory=directory)
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        yield f"http://127.0.0.1:{server.server_port}"
    finally:
        server.shutdown()
        thread.join()


class _LyingLengthHandler(http.server.BaseHTTPRequestHandler):
    """Serve a short body with a false Content-Length."""

    BODY = b"short"
    CLAIMED_LENGTH = 999

    def do_GET(self):
        self.send_response(200)
        self.send_header("Content-Length", str(self.CLAIMED_LENGTH))
        self.end_headers()
        self.wfile.write(self.BODY)

    def do_HEAD(self):
        self.send_response(200)
        self.send_header("Content-Length", str(self.CLAIMED_LENGTH))
        self.end_headers()

    def log_message(self, format, *args):
        pass


@contextlib.contextmanager
def lying_length_server():
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), _LyingLengthHandler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        yield f"http://127.0.0.1:{server.server_port}"
    finally:
        server.shutdown()
        thread.join()


@contextlib.contextmanager
def _patched(obj, name, value):
    original = getattr(obj, name)
    setattr(obj, name, value)
    try:
        yield
    finally:
        setattr(obj, name, original)


def test_download_tile_fresh():
    with tempfile.TemporaryDirectory() as served, tempfile.TemporaryDirectory() as dest_dir:
        content = b"elevation data goes here" * 100
        with open(os.path.join(served, "tile.tif"), "wb") as f:
            f.write(content)

        with file_server(served) as base_url:
            tile = SourceTile(url=f"{base_url}/tile.tif", local_name="tile.tif")
            dest_path = os.path.join(dest_dir, "tile.tif")

            result = download_tile(tile, dest_path)
            assert result == "downloaded"
            with open(dest_path, "rb") as f:
                assert f.read() == content
            assert not os.path.exists(dest_path + ".part")
    return True


def test_download_tile_skips_when_already_correct():
    with tempfile.TemporaryDirectory() as served, tempfile.TemporaryDirectory() as dest_dir:
        content = b"x" * 4096
        with open(os.path.join(served, "tile.tif"), "wb") as f:
            f.write(content)

        with file_server(served) as base_url:
            tile = SourceTile(url=f"{base_url}/tile.tif", local_name="tile.tif")
            dest_path = os.path.join(dest_dir, "tile.tif")

            assert download_tile(tile, dest_path) == "downloaded"
            assert download_tile(tile, dest_path) == "skipped"
    return True


def test_download_tile_redownloads_on_size_mismatch():
    with tempfile.TemporaryDirectory() as served, tempfile.TemporaryDirectory() as dest_dir:
        content = b"y" * 4096
        with open(os.path.join(served, "tile.tif"), "wb") as f:
            f.write(content)

        with file_server(served) as base_url:
            tile = SourceTile(url=f"{base_url}/tile.tif", local_name="tile.tif")
            dest_path = os.path.join(dest_dir, "tile.tif")

            # A truncated leftover from some earlier, interrupted run.
            with open(dest_path, "wb") as f:
                f.write(content[:100])

            result = download_tile(tile, dest_path)
            assert result == "downloaded"
            with open(dest_path, "rb") as f:
                assert f.read() == content
    return True


def test_download_tile_force_redownloads_even_if_correct():
    with tempfile.TemporaryDirectory() as served, tempfile.TemporaryDirectory() as dest_dir:
        content = b"z" * 2048
        with open(os.path.join(served, "tile.tif"), "wb") as f:
            f.write(content)

        with file_server(served) as base_url:
            tile = SourceTile(url=f"{base_url}/tile.tif", local_name="tile.tif")
            dest_path = os.path.join(dest_dir, "tile.tif")

            assert download_tile(tile, dest_path) == "downloaded"
            assert download_tile(tile, dest_path, force=True) == "downloaded"
    return True


def test_download_tile_404_raises_without_leaving_a_partial_file():
    with tempfile.TemporaryDirectory() as served, tempfile.TemporaryDirectory() as dest_dir:
        with file_server(served) as base_url:
            tile = SourceTile(url=f"{base_url}/missing.tif", local_name="missing.tif")
            dest_path = os.path.join(dest_dir, "missing.tif")

            try:
                download_tile(tile, dest_path)
                assert False, "expected an HTTPError"
            except urllib.error.HTTPError:
                pass
            assert not os.path.exists(dest_path)
            assert not os.path.exists(dest_path + ".part")
    return True


def test_download_tile_size_verification_raises_and_cleans_up():
    with tempfile.TemporaryDirectory() as dest_dir:
        with lying_length_server() as base_url:
            tile = SourceTile(url=f"{base_url}/tile.tif", local_name="tile.tif")
            dest_path = os.path.join(dest_dir, "tile.tif")

            try:
                download_tile(tile, dest_path)
                assert False, "expected a ValueError"
            except ValueError as e:
                assert "expected 999" in str(e)
            assert not os.path.exists(dest_path)
            assert not os.path.exists(dest_path + ".part")
    return True


def test_main_end_to_end():
    """The CLI downloads and then reuses a source file."""
    adapter = get_adapter("gmted2010")
    filename = adapter.source_tiles()[0].local_name

    with tempfile.TemporaryDirectory() as served, tempfile.TemporaryDirectory() as dest_dir:
        content = b"fake global grid zip bytes" * 10
        with open(os.path.join(served, filename), "wb") as f:
            f.write(content)

        with file_server(served) as base_url, _patched(Gmted2010Adapter, "SOURCE_BASE_URL", base_url):
            argv = ["--dataset", "gmted2010", dest_dir]
            run_download_terrain(argv)  # fresh download
            run_download_terrain(argv)  # resumable: should skip without error

            dest_path = os.path.join(dest_dir, filename)
            with open(dest_path, "rb") as f:
                assert f.read() == content
    return True


TESTS = [
    test_download_tile_fresh,
    test_download_tile_skips_when_already_correct,
    test_download_tile_redownloads_on_size_mismatch,
    test_download_tile_force_redownloads_even_if_correct,
    test_download_tile_404_raises_without_leaving_a_partial_file,
    test_download_tile_size_verification_raises_and_cleans_up,
    test_main_end_to_end,
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
