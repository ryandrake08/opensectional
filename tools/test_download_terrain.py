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

import http_retry
from download_terrain import download_tile, main as run_download_terrain, parse_bbox
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


class _No500OnHeadHandler(http.server.SimpleHTTPRequestHandler):
    """Serves GET normally but fails every HEAD, so a test can prove a
    code path never issues one."""

    def do_HEAD(self):
        self.send_error(500, "HEAD should not have been called")

    def log_message(self, format, *args):
        pass


def test_download_tile_skip_on_existence_issues_no_head():
    with tempfile.TemporaryDirectory() as served, tempfile.TemporaryDirectory() as dest_dir:
        with open(os.path.join(served, "tile.tif"), "wb") as f:
            f.write(b"data" * 100)
        handler = functools.partial(_No500OnHeadHandler, directory=served)
        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            base_url = f"http://127.0.0.1:{server.server_port}"
            tile = SourceTile(url=f"{base_url}/tile.tif", local_name="tile.tif")
            dest_path = os.path.join(dest_dir, "tile.tif")

            assert download_tile(tile, dest_path, verify_size=False) == "downloaded"
            # A verify_size=True skip would HEAD and hit the 500; verify_size=False
            # must skip on existence alone.
            assert download_tile(tile, dest_path, verify_size=False) == "skipped"
        finally:
            server.shutdown()
            thread.join()
    return True


_flaky_hits = {}


class _FlakyHandler(http.server.BaseHTTPRequestHandler):
    """503s the first two GETs for any path, then serves BODY. Mimics
    S3 throttling a burst of concurrent requests."""

    BODY = b"tile bytes" * 50

    def do_GET(self):
        n = _flaky_hits.get(self.path, 0) + 1
        _flaky_hits[self.path] = n
        if n <= 2:
            self.send_error(503, "SlowDown")
            return
        self.send_response(200)
        self.send_header("Content-Length", str(len(self.BODY)))
        self.end_headers()
        self.wfile.write(self.BODY)

    def log_message(self, format, *args):
        pass


def test_download_tile_retries_transient_errors():
    _flaky_hits.clear()
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), _FlakyHandler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    with _patched(http_retry.time, "sleep", lambda *a: None):
        try:
            base_url = f"http://127.0.0.1:{server.server_port}"
            with tempfile.TemporaryDirectory() as dest_dir:
                tile = SourceTile(url=f"{base_url}/t.tif", local_name="t.tif")
                dest_path = os.path.join(dest_dir, "t.tif")
                assert download_tile(tile, dest_path, verify_size=False) == "downloaded"
                with open(dest_path, "rb") as f:
                    assert f.read() == _FlakyHandler.BODY
                assert not os.path.exists(dest_path + ".part")
        finally:
            server.shutdown()
            thread.join()
    return True


def test_download_tile_gives_up_after_the_retry_budget():
    """A permanently failing endpoint raises rather than looping forever,
    and leaves no partial file."""
    class _Always503(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            self.send_error(503)
        def log_message(self, format, *args):
            pass

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), _Always503)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    with _patched(http_retry.time, "sleep", lambda *a: None):
        try:
            base_url = f"http://127.0.0.1:{server.server_port}"
            with tempfile.TemporaryDirectory() as dest_dir:
                tile = SourceTile(url=f"{base_url}/t.tif", local_name="t.tif")
                dest_path = os.path.join(dest_dir, "t.tif")
                try:
                    download_tile(tile, dest_path, verify_size=False)
                    assert False, "expected an HTTPError"
                except urllib.error.HTTPError as e:
                    assert e.code == 503
                assert not os.path.exists(dest_path + ".part")
        finally:
            server.shutdown()
            thread.join()
    return True


def test_parse_bbox_valid_and_invalid():
    assert parse_bbox("-125,24,-66,50") == (-125.0, 24.0, -66.0, 50.0)
    assert parse_bbox("170,-20,-170,20") == (170.0, -20.0, -170.0, 20.0)  # antimeridian wrap
    for bad in ("1,2,3", "a,b,c,d", "0,50,10,20", "0,-30,0,120", "-190,0,10,10"):
        try:
            parse_bbox(bad)
            assert False, f"expected ValueError for {bad!r}"
        except ValueError:
            pass
    return True


def test_main_copernicus_parallel_download_and_resume():
    """A per-tile dataset fetches every listed tile in parallel, then a
    second run skips them all on existence."""
    adapter = get_adapter("copernicus-glo90")
    names = [
        "Copernicus_DSM_COG_30_N39_00_W120_00_DEM",
        "Copernicus_DSM_COG_30_N40_00_W120_00_DEM",
        "Copernicus_DSM_COG_30_N45_00_W110_00_DEM",
    ]
    with tempfile.TemporaryDirectory() as served, tempfile.TemporaryDirectory() as dest_dir:
        with open(os.path.join(served, "tileList.txt"), "w") as f:
            f.write("\n".join(names) + "\n")
        for name in names:
            os.makedirs(os.path.join(served, name))
            with open(os.path.join(served, name, f"{name}.tif"), "wb") as f:
                f.write(f"fake COG {name}".encode())

        with file_server(served) as base_url, _patched(adapter, "bucket_url", base_url):
            argv = ["--dataset", "copernicus-glo90", "--jobs", "3", dest_dir]
            run_download_terrain(argv)
            for name in names:
                assert os.path.exists(os.path.join(dest_dir, f"{name}.tif"))

            # Second run: all present, skipped without error.
            run_download_terrain(argv)

            # --bbox trims what gets requested.
            run_download_terrain(["--dataset", "copernicus-glo90",
                                  "--bbox=-115,38,-105,50", dest_dir])
    return True


def test_main_rejects_non_positive_jobs():
    for bad in ("0", "-4"):
        try:
            run_download_terrain(["--dataset", "gmted2010-30", "--jobs", bad, "/tmp/unused"])
            assert False, f"expected SystemExit for --jobs {bad}"
        except SystemExit as e:
            assert e.code == 2
    return True


def test_main_bounds_the_submission_backlog():
    """A mid-run failure must not have already queued every remaining
    tile -- only a small backlog is submitted at a time."""
    import download_terrain

    names = [f"Copernicus_DSM_COG_30_N{lat:02d}_00_W100_00_DEM" for lat in range(10, 40)]
    started = []
    lock = threading.Lock()

    def flaky_download_tile(tile, dest_path, **kwargs):
        with lock:
            started.append(tile.local_name)
        if tile.local_name == names[1] + ".tif":
            raise urllib.error.HTTPError(tile.url, 404, "Not Found", None, None)
        open(dest_path, "wb").close()
        return "downloaded"

    def fake_source_tiles(bbox=None):
        return [SourceTile(url=f"http://x/{n}/{n}.tif", local_name=f"{n}.tif") for n in names]

    adapter = get_adapter("copernicus-glo90")
    with tempfile.TemporaryDirectory() as dest_dir, \
         _patched(download_terrain, "download_tile", flaky_download_tile), \
         _patched(adapter, "source_tiles", fake_source_tiles):
        try:
            run_download_terrain(["--dataset", "copernicus-glo90", "--jobs", "2", dest_dir])
            assert False, "expected the 404 to propagate"
        except urllib.error.HTTPError:
            pass

    assert len(started) < len(names), f"submitted all {len(names)} tiles despite an early failure"
    assert len(started) <= 10, f"backlog larger than expected: {len(started)}"
    return True


def test_main_end_to_end():
    """The CLI downloads and then reuses a source file."""
    adapter = get_adapter("gmted2010-30")
    filename = adapter.source_tiles()[0].local_name

    with tempfile.TemporaryDirectory() as served, tempfile.TemporaryDirectory() as dest_dir:
        content = b"fake global grid zip bytes" * 10
        with open(os.path.join(served, filename), "wb") as f:
            f.write(content)

        with file_server(served) as base_url, _patched(Gmted2010Adapter, "SOURCE_BASE_URL", base_url):
            argv = ["--dataset", "gmted2010-30", dest_dir]
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
    test_download_tile_skip_on_existence_issues_no_head,
    test_download_tile_retries_transient_errors,
    test_download_tile_gives_up_after_the_retry_budget,
    test_parse_bbox_valid_and_invalid,
    test_main_rejects_non_positive_jobs,
    test_main_bounds_the_submission_backlog,
    test_main_copernicus_parallel_download_and_resume,
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
