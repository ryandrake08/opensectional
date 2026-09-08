#!/usr/bin/env bash
# check-all.sh — static checks for the Python tools in tools/.
#
# Runs ruff (lint, ruff.toml) and basedpyright (types, standard mode,
# pyrightconfig.json) against tools/. Both are in tools/requirements.txt.
#
# Run it from an activated venv built from that file — the venv name and
# platform don't matter, only that it's the active one:
#   source tools/<your-venv>/bin/activate
#   tools/check-all.sh
set -uo pipefail

cd "$(dirname "$0")/.."

if [ -z "${VIRTUAL_ENV:-}" ]; then
    echo "check-all: no active virtualenv." >&2
    echo "  source tools/<your-venv>/bin/activate   # built from tools/requirements.txt" >&2
    exit 1
fi

for tool in ruff basedpyright; do
    if [ ! -x "${VIRTUAL_ENV}/bin/${tool}" ]; then
        echo "check-all: ${tool} missing from ${VIRTUAL_ENV}." >&2
        echo "  pip install -r tools/requirements.txt" >&2
        exit 1
    fi
done

rc=0

echo "=== ruff ==="
"${VIRTUAL_ENV}/bin/ruff" check tools/ || rc=1

# basedpyright reads $VIRTUAL_ENV to resolve third-party imports.
echo "=== basedpyright ==="
"${VIRTUAL_ENV}/bin/basedpyright" || rc=1

exit "${rc}"
