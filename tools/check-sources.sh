#!/bin/bash
# Check project-owned C and C++ source files with clangd.
# Usage: tools/check-sources.sh [-j N] [-t] [-v] [-c DIR] [directory...]
# -j N: Run N checks in parallel (default: number of CPU cores)
# -t: Run standalone clang-tidy checks
# -v: Verbose mode - show full clangd output for each file
# -c DIR: Use compile_commands.json from DIR (default: build/)
# Without directories, checks src/, lib/, and shaders/.

set -o pipefail

cd "$(dirname "$0")/.." || exit 1

# Find the best available clangd.
# Priority: CLANGD env var > newest MacPorts clangd > clangd on PATH.
find_clangd() {
    if [ -n "${CLANGD:-}" ]; then
        local configured_clangd
        configured_clangd=$(command -v "$CLANGD" 2>/dev/null || true)
        if [ -n "$configured_clangd" ] && [ -x "$configured_clangd" ]; then
            echo "$configured_clangd"
            return
        fi
        echo "Warning: CLANGD=$CLANGD is not executable, searching for alternatives" >&2
    fi

    get_clangd_major() {
        local version
        version=$("$1" --version 2>&1 | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1)
        echo "${version%%.*}"
    }

    local system_clangd
    system_clangd=$(command -v clangd 2>/dev/null || true)
    local system_major=0
    if [ -n "$system_clangd" ]; then
        system_major=$(get_clangd_major "$system_clangd")
        case $system_major in
            ''|*[!0-9]*) system_major=0 ;;
        esac
    fi

    if [ "$(uname)" = "Darwin" ]; then
        local best_clangd=""
        local best_major=$system_major
        local candidate
        local candidate_major
        local candidates=(/opt/local/bin/clangd)

        for candidate in /opt/local/bin/clangd-mp-*; do
            [ -x "$candidate" ] && candidates+=("$candidate")
        done

        for candidate in "${candidates[@]}"; do
            if [ -x "$candidate" ]; then
                candidate_major=$(get_clangd_major "$candidate")
                case $candidate_major in
                    ''|*[!0-9]*) candidate_major=0 ;;
                esac
                if [ "$candidate_major" -gt "$best_major" ]; then
                    best_major=$candidate_major
                    best_clangd=$candidate
                fi
            fi
        done

        if [ -n "$best_clangd" ]; then
            echo "Note: clangd on PATH is v$system_major, using MacPorts v$best_major: $best_clangd" >&2
            echo "$best_clangd"
            return
        fi
    fi

    if [ -n "$system_clangd" ]; then
        echo "$system_clangd"
        return
    fi

    return 1
}

CLANGD_BIN=$(find_clangd)
if [ -z "$CLANGD_BIN" ]; then
    echo "Error: clangd not found. Install clangd or set CLANGD." >&2
    exit 1
fi

clangd_version=$("$CLANGD_BIN" --version 2>&1 | head -1)
jobs=$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)

verbose=0
run_clang_tidy=0
compile_commands_dir=build
while getopts "j:tvc:" opt; do
    case $opt in
        j) jobs=$OPTARG ;;
        t) run_clang_tidy=1 ;;
        v) verbose=1 ;;
        c) compile_commands_dir=$OPTARG ;;
        *)
            echo "Usage: $0 [-j N] [-t] [-v] [-c DIR] [directory...]"
            exit 1
            ;;
    esac
done
shift $((OPTIND - 1))

if [ "$run_clang_tidy" = "1" ]; then
    if [ -n "${CLANG_TIDY:-}" ]; then
        CLANG_TIDY_BIN=$(command -v "$CLANG_TIDY" 2>/dev/null || true)
    else
        CLANG_TIDY_BIN="${CLANGD_BIN/clangd/clang-tidy}"
        if [ ! -x "$CLANG_TIDY_BIN" ]; then
            CLANG_TIDY_BIN=$(command -v clang-tidy 2>/dev/null || true)
        fi
    fi
    if [ -z "${CLANG_TIDY_BIN:-}" ] || [ ! -x "$CLANG_TIDY_BIN" ]; then
        echo "Error: clang-tidy not found. Install it or set CLANG_TIDY." >&2
        exit 1
    fi
fi

case $jobs in
    ''|*[!0-9]*|0)
        echo "Error: -j must be a positive integer" >&2
        exit 1
        ;;
esac

if [ ! -f "$compile_commands_dir/compile_commands.json" ]; then
    echo "Error: $compile_commands_dir/compile_commands.json not found." >&2
    echo "Configure the project first, for example: cmake --preset release" >&2
    exit 1
fi
compile_commands_dir=$(cd "$compile_commands_dir" && pwd)

if [ $# -eq 0 ]; then
    dirs=(src lib shaders)
else
    dirs=("$@")
fi

for dir in "${dirs[@]}"; do
    if [ ! -d "$dir" ]; then
        echo "Error: source directory not found: $dir" >&2
        exit 1
    fi
done

results_file=$(mktemp)
file_list=$(mktemp)
trap 'rm -f "$results_file" "$file_list"' EXIT

check_file() {
    local verbose=$1
    local compile_commands_dir=$2
    local clangd_bin=$3
    local run_clang_tidy=$4
    local clang_tidy_bin=$5
    local f=$6
    local output
    local diagnostics
    local tool_errors
    local clangd_args=(--check="$f" --compile-commands-dir="$compile_commands_dir")

    output=$("$clangd_bin" "${clangd_args[@]}" 2>&1 || true)
    diagnostics=$(printf '%s\n' "$output" | grep -E '(:[0-9]+:[0-9]+: (error|warning):|^E\[.*\] \[.*\] Line [0-9]+:)' | grep -v -E 'in included file|\[fatal_too_many_errors\]' || true)
    tool_errors=$(printf '%s\n' "$output" | grep -E 'tidy-config error|Error parsing clang-tidy configuration|Failed to (find|load) compilation database|Could not auto-detect compilation database|\[fatal_too_many_errors\]' || true)

    if [ "$verbose" = "1" ]; then
        echo ""
        echo "--- $f (verbose) ---"
        echo "$output"
        echo "--- end ---"
    fi

    if [ -n "$diagnostics" ]; then
        echo ""
        echo "=== $f ==="
        echo "$diagnostics"
    fi

    if [ -n "$tool_errors" ]; then
        echo ""
        echo "=== $f (clangd failure) ==="
        echo "$tool_errors"
    fi

    if [ "$run_clang_tidy" = "1" ]; then
        local tidy_output
        local tidy_diagnostics
        tidy_output=$("$clang_tidy_bin" -p "$compile_commands_dir" "$f" 2>&1 || true)
        tidy_diagnostics=$(printf '%s\n' "$tidy_output" | grep -E ':[0-9]+:[0-9]+: (error|warning):' || true)

        if [ "$verbose" = "1" ]; then
            echo ""
            echo "--- $f (clang-tidy verbose) ---"
            echo "$tidy_output"
            echo "--- end ---"
        fi

        if [ -n "$tidy_diagnostics" ]; then
            echo ""
            echo "=== $f (clang-tidy) ==="
            echo "$tidy_diagnostics"
        fi
    fi
}
export -f check_file

echo "Checking source files with clangd (parallel: $jobs jobs)"
echo "Using: $clangd_version"
echo "Binary: $CLANGD_BIN"
echo "Compile commands: $compile_commands_dir"
if [ "$run_clang_tidy" = "1" ]; then
    echo "Clang-tidy: $CLANG_TIDY_BIN"
fi
echo "Directories: ${dirs[*]}"
echo "==========================================="

for dir in "${dirs[@]}"; do
    find "$dir" -type f \( -name '*.c' -o -name '*.cc' -o -name '*.cpp' -o -name '*.cxx' \) \
        ! -name '._*' -print0 >> "$file_list"
done

files_count=$(tr -cd '\0' < "$file_list" | wc -c | tr -d '[:space:]')
echo "Found $files_count source files to check"

if [ "$files_count" -gt 0 ]; then
    xargs -0 -P "$jobs" -n 1 bash -c 'check_file "$@"' _ \
        "$verbose" "$compile_commands_dir" "$CLANGD_BIN" "$run_clang_tidy" "$CLANG_TIDY_BIN" < "$file_list" > "$results_file"
fi

cat "$results_file"

issues_count=$(grep -c '^===' "$results_file" 2>/dev/null || true)
issues_count=${issues_count:-0}

echo ""
echo "==========================================="
echo "Checked $files_count files"
if [ "$issues_count" -eq 0 ]; then
    echo "No code issues found."
else
    echo "Found issues in $issues_count file(s)"
    exit 1
fi
