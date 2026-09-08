#!/usr/bin/env python3
"""Tests for the basemap source download using a local HTTP server."""

import contextlib
import functools
import http.server
import os
import sys
import tempfile
import threading
import urllib.error

import download_basemap
from download_basemap import download, main as run_download_basemap


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


def test_download_fresh():
    with tempfile.TemporaryDirectory() as served, tempfile.TemporaryDirectory() as dest_dir:
        content = b"natural earth bytes" * 100
        with open(os.path.join(served, "ne.zip"), "wb") as f:
            f.write(content)

        with file_server(served) as base_url:
            dest_path = os.path.join(dest_dir, "ne.zip")
            assert download(f"{base_url}/ne.zip", dest_path) == "downloaded"
            with open(dest_path, "rb") as f:
                assert f.read() == content
            assert not os.path.exists(dest_path + ".part")
    return True


def test_download_skips_when_already_correct():
    with tempfile.TemporaryDirectory() as served, tempfile.TemporaryDirectory() as dest_dir:
        with open(os.path.join(served, "ne.zip"), "wb") as f:
            f.write(b"x" * 4096)

        with file_server(served) as base_url:
            dest_path = os.path.join(dest_dir, "ne.zip")
            assert download(f"{base_url}/ne.zip", dest_path) == "downloaded"
            assert download(f"{base_url}/ne.zip", dest_path) == "skipped"
    return True


def test_download_force_redownloads_even_if_correct():
    with tempfile.TemporaryDirectory() as served, tempfile.TemporaryDirectory() as dest_dir:
        with open(os.path.join(served, "ne.zip"), "wb") as f:
            f.write(b"z" * 4096)

        with file_server(served) as base_url:
            dest_path = os.path.join(dest_dir, "ne.zip")
            assert download(f"{base_url}/ne.zip", dest_path) == "downloaded"
            assert download(f"{base_url}/ne.zip", dest_path, force=True) == "downloaded"
    return True


def test_download_404_raises_without_leaving_a_partial_file():
    with tempfile.TemporaryDirectory() as served, tempfile.TemporaryDirectory() as dest_dir:
        with file_server(served) as base_url:
            dest_path = os.path.join(dest_dir, "missing.zip")
            try:
                download(f"{base_url}/missing.zip", dest_path)
                assert False, "expected an HTTPError"
            except urllib.error.HTTPError:
                pass
            assert not os.path.exists(dest_path)
            assert not os.path.exists(dest_path + ".part")
    return True


def test_download_size_verification_raises_and_cleans_up():
    with tempfile.TemporaryDirectory() as dest_dir:
        with lying_length_server() as base_url:
            dest_path = os.path.join(dest_dir, "ne.zip")
            try:
                download(f"{base_url}/ne.zip", dest_path)
                assert False, "expected a ValueError"
            except ValueError as e:
                assert "expected 999" in str(e)
            assert not os.path.exists(dest_path)
            assert not os.path.exists(dest_path + ".part")
    return True


def test_main_end_to_end():
    """The CLI downloads into output_dir and then skips on a second run."""
    with tempfile.TemporaryDirectory() as served, tempfile.TemporaryDirectory() as dest_dir:
        content = b"fake geopackage zip bytes" * 10
        with open(os.path.join(served, download_basemap.LOCAL_NAME), "wb") as f:
            f.write(content)

        with file_server(served) as base_url:
            url = f"{base_url}/{download_basemap.LOCAL_NAME}"
            original = download_basemap.SOURCE_URL
            download_basemap.SOURCE_URL = url
            try:
                run_download_basemap([dest_dir])  # fresh download
                run_download_basemap([dest_dir])  # resumable: skips without error
            finally:
                download_basemap.SOURCE_URL = original

        with open(os.path.join(dest_dir, download_basemap.LOCAL_NAME), "rb") as f:
            assert f.read() == content
    return True


TESTS = [
    test_download_fresh,
    test_download_skips_when_already_correct,
    test_download_force_redownloads_even_if_correct,
    test_download_404_raises_without_leaving_a_partial_file,
    test_download_size_verification_raises_and_cleans_up,
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
