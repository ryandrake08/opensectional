#!/bin/bash
# Build a self-contained Windows NSIS installer for OpenSectional via
# MinGW-w64 cross-compile.
#
# Pulls all dependency submodules, downloads the SQLite amalgamation,
# configures CMake with OSECT_VENDOR_DEPS=ON so SDL3 / SDL3_image /
# SDL3_ttf / curl / zlib / sqlite are all built from in-tree sources,
# builds osect.exe with Schannel TLS, and runs cpack to produce an
# NSIS installer.
#
# Run on a Linux/macOS host that has the MinGW-w64 cross-toolchain
# installed (Ubuntu: mingw-w64; macOS: brew install mingw-w64). Also
# requires NSIS for the installer step (Ubuntu: nsis; macOS: brew
# install makensis).
#
# After a successful cpack the script restores thirdparty/ to its
# pre-build state (deinits the dependency submodules, removes the
# extracted SQLite amalgamation, clears the tarball cache). Pass
# --no-clean to keep the build state in place — useful when iterating
# on the installer where you don't want to re-init the submodule
# trees on every run.
#
# Usage:
#   ./tools/build-mingw-package.sh [--no-clean]
set -euo pipefail

CLEAN_AFTER=1
for arg in "$@"; do
    case "${arg}" in
        --no-clean) CLEAN_AFTER=0 ;;
        -h|--help)
            sed -n '2,/^set -/p' "$0" | sed 's/^# \{0,1\}//;$d'
            exit 0 ;;
        *) echo "ERROR: unknown argument: ${arg}" >&2; exit 1 ;;
    esac
done

PROJECTDIR="$(cd "$(dirname "$0")/.." && pwd)"
THIRDPARTY="${PROJECTDIR}/thirdparty"
BUILDDIR="${PROJECTDIR}/build-mingw-package"

SQLITE_TARBALL="sqlite-autoconf-3490100.tar.gz"
SQLITE_URL="https://www.sqlite.org/2025/${SQLITE_TARBALL}"
SQLITE_SHA256="106642d8ccb36c5f7323b64e4152e9b719f7c0215acf5bfeac3d5e7f97b59254"

if ! command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1; then
    echo "ERROR: x86_64-w64-mingw32-gcc not found." >&2
    echo "Install the MinGW-w64 cross-toolchain (Ubuntu: mingw-w64; macOS: brew install mingw-w64)." >&2
    exit 1
fi

sha256() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
    else
        shasum -a 256 "$1" | awk '{print $1}'
    fi
}

download_verify_extract() {
    local url="$1" sha="$2" dest="$3"
    local cache="${THIRDPARTY}/.cache"
    local file="${cache}/$(basename "$url")"
    mkdir -p "$cache"
    if [ ! -f "$file" ]; then
        echo "  fetching $(basename "$url")..."
        curl -fSL --output "$file" "$url"
    fi
    local actual
    actual=$(sha256 "$file")
    if [ "$actual" != "$sha" ]; then
        echo "ERROR: sha256 mismatch for ${file}" >&2
        echo "  expected: ${sha}" >&2
        echo "  actual:   ${actual}" >&2
        exit 1
    fi
    mkdir -p "$dest"
    case "$file" in
        *.tar.gz|*.tgz) tar xzf "$file" -C "$dest" --strip-components=1 ;;
        *.tar)          tar xf  "$file" -C "$dest" ;;
        *) echo "ERROR: unknown archive type: $file" >&2; exit 1 ;;
    esac
}

if command -v nproc >/dev/null 2>&1; then
    JOBS=$(nproc)
else
    JOBS=$(sysctl -n hw.ncpu)
fi

echo "=== Initializing dependency submodules ==="
git -C "${PROJECTDIR}" submodule update --init --recursive \
    thirdparty/SDL thirdparty/SDL_image thirdparty/SDL_ttf \
    thirdparty/zlib thirdparty/curl

if [ ! -f "${THIRDPARTY}/sqlite/sqlite3.c" ]; then
    echo "=== Fetching SQLite amalgamation ==="
    download_verify_extract "${SQLITE_URL}" "${SQLITE_SHA256}" "${THIRDPARTY}/sqlite"
fi

echo "=== Configuring (MinGW-w64 cross, OSECT_VENDOR_DEPS=ON) ==="
cmake -B "${BUILDDIR}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="${PROJECTDIR}/cmake/mingw-w64-toolchain.cmake" \
    -DOSECT_VENDOR_DEPS=ON

echo "=== Building ==="
cmake --build "${BUILDDIR}" -j"${JOBS}"

echo "=== Packaging (cpack → NSIS) ==="
( cd "${BUILDDIR}" && cpack )

if [ "${CLEAN_AFTER}" -eq 1 ]; then
    echo "=== Restoring thirdparty/ to pre-build state ==="
    # Deinit empties the submodule working trees but leaves
    # .git/modules/ caches in place so a future re-init is fast.
    git -C "${PROJECTDIR}" submodule deinit -f -- \
        thirdparty/SDL thirdparty/SDL_image thirdparty/SDL_ttf \
        thirdparty/zlib thirdparty/curl 2>&1 | sed 's/^/  /' || true
    # thirdparty/sqlite/ and thirdparty/.cache/ are entirely tarball-
    # extracted state — neither path exists in a fresh clone.
    rm -rf "${THIRDPARTY}/sqlite" \
           "${THIRDPARTY}/.cache"
    # Sanity-check: only flag working-tree changes (column 2 of porcelain),
    # not index-only entries like "A " for staged-but-uncommitted submodule
    # pointers on a feature branch.
    leftover=$(
        git -C "${PROJECTDIR}" status --porcelain -- thirdparty/ 2>/dev/null \
            | awk 'substr($0, 2, 1) != " "' \
            || true
    )
    if [ -n "${leftover}" ]; then
        echo "  WARNING: working-tree changes remain under thirdparty/:"
        echo "${leftover}" | sed 's/^/    /'
    fi
fi

echo ""
echo "=== Done. ==="
ls -1 "${BUILDDIR}"/*.exe 2>/dev/null || true
if [ "${CLEAN_AFTER}" -eq 0 ]; then
    echo ""
    echo "thirdparty/ left initialized (--no-clean). Re-run without the"
    echo "flag, or remove the build-mingw-package/ directory and the"
    echo "submodule/tarball state under thirdparty/ manually."
fi
