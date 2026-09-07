#!/usr/bin/env python3
"""Tests for terrain datum conversion helpers."""

import contextlib
import functools
import http.server
import os
import sys
import tempfile
import threading

import terrain_datum
from terrain_datum import to_egm2008


class _QuietHandler(http.server.SimpleHTTPRequestHandler):
    def log_message(self, format, *args):
        pass


@contextlib.contextmanager
def file_server(directory):
    handler = functools.partial(_QuietHandler, directory=directory)
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
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


def test_identity_for_target_datum():
    assert to_egm2008(123.456, -122.4, 37.6, "EGM2008") == 123.456
    return True


def test_unknown_datum_raises():
    try:
        to_egm2008(100.0, 0.0, 0.0, "NAVD88")
        assert False, "expected ValueError"
    except ValueError as e:
        assert "EGM96" in str(e)
    return True


def test_ensure_grid_fetches_and_resumes():
    """Grid downloads are cached."""
    with tempfile.TemporaryDirectory() as served, tempfile.TemporaryDirectory() as cache_dir:
        content = b"fake grid bytes" * 1000
        with open(os.path.join(served, "fake_grid.tif"), "wb") as f:
            f.write(content)

        with file_server(served) as base_url:
            with _patched(terrain_datum, "_PROJ_CDN", base_url), \
                 _patched(terrain_datum, "GRID_CACHE_DIR", cache_dir):
                path = terrain_datum._ensure_grid("fake_grid.tif")
                with open(path, "rb") as f:
                    assert f.read() == content

                # Second call should skip re-downloading (same file, same size).
                os.utime(path, (0, 0))  # detectable if it got overwritten
                terrain_datum._ensure_grid("fake_grid.tif")
                assert os.stat(path).st_mtime == 0
    return True


def test_transformer_for_is_cached():
    """Transformers are cached per datum."""
    ensure_calls = []
    pipeline_calls = []

    def fake_ensure_grid(filename):
        ensure_calls.append(filename)
        return filename

    class FakeTransformer:
        def transform(self, lon, lat, z):
            # Matches the EGM96 canary's expected delta so the
            # correctness check _transformer_for runs internally passes.
            return (lon, lat, -0.0652)

    def fake_from_pipeline(pipeline):
        pipeline_calls.append(pipeline)
        return FakeTransformer()

    terrain_datum._transformer_cache.clear()
    with _patched(terrain_datum, "_ensure_grid", fake_ensure_grid), \
         _patched(terrain_datum.Transformer, "from_pipeline", staticmethod(fake_from_pipeline)):
        t1 = terrain_datum._transformer_for("EGM96")
        t2 = terrain_datum._transformer_for("EGM96")

    assert t1 is t2
    assert len(pipeline_calls) == 1
    assert ensure_calls.count("us_nga_egm96_15.tif") == 1
    assert ensure_calls.count("us_nga_egm08_25.tif") == 1
    terrain_datum._transformer_cache.clear()
    return True


def test_pipeline_string_has_correct_grids_and_multipliers():
    """The pipeline uses the expected grid order and signs."""
    captured = {}

    def fake_ensure_grid(filename):
        return filename

    class FakeTransformer:
        def transform(self, lon, lat, z):
            return (lon, lat, -0.0652)  # matches the EGM96 canary's expected delta

    def fake_from_pipeline(pipeline):
        captured["pipeline"] = pipeline
        return FakeTransformer()

    terrain_datum._transformer_cache.clear()
    with _patched(terrain_datum, "_ensure_grid", fake_ensure_grid), \
         _patched(terrain_datum.Transformer, "from_pipeline", staticmethod(fake_from_pipeline)):
        terrain_datum._transformer_for("EGM96")

    pipeline = captured["pipeline"]
    assert "+grids=us_nga_egm96_15.tif +multiplier=1" in pipeline
    assert "+grids=us_nga_egm08_25.tif +multiplier=-1" in pipeline
    assert pipeline.index("egm96_15") < pipeline.index("egm08_25")
    terrain_datum._transformer_cache.clear()
    return True


def test_canary_catches_a_silent_noop_transform():
    """The canary rejects an identity transform."""
    class NoOpTransformer:
        def transform(self, lon, lat, z):
            return (lon, lat, z)  # unchanged -- exactly what a silent fallback does

    def fake_ensure_grid(filename):
        return filename

    def fake_from_pipeline(pipeline):
        return NoOpTransformer()

    terrain_datum._transformer_cache.clear()
    with _patched(terrain_datum, "_ensure_grid", fake_ensure_grid), \
         _patched(terrain_datum.Transformer, "from_pipeline", staticmethod(fake_from_pipeline)):
        try:
            terrain_datum._transformer_for("EGM96")
            assert False, "expected a RuntimeError from the correctness check"
        except RuntimeError as e:
            assert "correctness check" in str(e)
    assert "EGM96" not in terrain_datum._transformer_cache, "a failed check must not be cached"
    terrain_datum._transformer_cache.clear()
    return True


def test_canary_catches_an_implausibly_large_shift():
    """The canary rejects implausible shifts."""
    class WildTransformer:
        def transform(self, lon, lat, z):
            return (lon, lat, z + 5000.0)  # no real geoid difference is this large

    def fake_ensure_grid(filename):
        return filename

    def fake_from_pipeline(pipeline):
        return WildTransformer()

    terrain_datum._transformer_cache.clear()
    with _patched(terrain_datum, "_ensure_grid", fake_ensure_grid), \
         _patched(terrain_datum.Transformer, "from_pipeline", staticmethod(fake_from_pipeline)):
        try:
            terrain_datum._transformer_for("EGM96")
            assert False, "expected a RuntimeError from the correctness check"
        except RuntimeError:
            pass
    terrain_datum._transformer_cache.clear()
    return True


def test_verify_datum_conversion_does_not_leave_a_cached_transformer():
    """Parent validation clears the transformer cache."""
    class FakeTransformer:
        def transform(self, lon, lat, z):
            return (lon, lat, -0.0652)

    def fake_ensure_grid(filename):
        return filename

    def fake_from_pipeline(pipeline):
        return FakeTransformer()

    terrain_datum._transformer_cache.clear()
    with _patched(terrain_datum, "_ensure_grid", fake_ensure_grid), \
         _patched(terrain_datum.Transformer, "from_pipeline", staticmethod(fake_from_pipeline)):
        terrain_datum.verify_datum_conversion("EGM96")
    assert "EGM96" not in terrain_datum._transformer_cache

    # A target-datum dataset needs no grids at all -- must not raise.
    terrain_datum.verify_datum_conversion("EGM2008")
    return True


TESTS = [
    test_identity_for_target_datum,
    test_unknown_datum_raises,
    test_ensure_grid_fetches_and_resumes,
    test_transformer_for_is_cached,
    test_pipeline_string_has_correct_grids_and_multipliers,
    test_canary_catches_a_silent_noop_transform,
    test_canary_catches_an_implausibly_large_shift,
    test_verify_datum_conversion_does_not_leave_a_cached_transformer,
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
