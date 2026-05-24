#!/usr/bin/env bash
#
# tools/coverage.sh — generate a code coverage report for GtkHx.
#
# Usage:
#   tools/coverage.sh                  # full run: Tier 1+2+3, HTML report
#   tools/coverage.sh --quick          # Tier 1+2 only (no Docker required)
#   tools/coverage.sh --open           # open HTML report when done
#   tools/coverage.sh --reset          # wipe build-cov/ and reconfigure
#   tools/coverage.sh --help
#
# Output: coverage/index.html at the project root.
#
# Requires: meson, ninja, gcc/clang (whatever meson picks), gcov,
# and either gcovr (preferred) or lcov+genhtml. Install with:
#   Fedora: dnf install gcovr
#   Debian: apt install gcovr
#   macOS:  brew install gcovr
#
# Tier 3 tests skip cleanly when the Docker test servers aren't
# reachable — see tests/mhxd/README.md and tests/janus/README.md
# for bring-up. Skipped tests don't contribute coverage, so for the
# fullest picture you want both containers running on the
# conventional ports (5500/5501 for mhxd, 5510/5511 for Janus)
# before invoking this script.

set -euo pipefail

# Resolve project root relative to this script so it works from any
# cwd.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$PROJECT_ROOT/build-cov"
REPORT_DIR="$PROJECT_ROOT/coverage"

QUICK=0
OPEN=0
RESET=0

usage() {
    sed -n '3,25p' "$0"
    exit "${1:-0}"
}

for arg in "$@"; do
    case "$arg" in
        --quick)  QUICK=1 ;;
        --open)   OPEN=1 ;;
        --reset)  RESET=1 ;;
        -h|--help) usage 0 ;;
        *) echo "unknown flag: $arg" >&2; usage 2 ;;
    esac
done

# Sanity check the tools first — failing here is cheaper than after
# a multi-minute build.
need() {
    command -v "$1" >/dev/null 2>&1 || {
        echo "error: $1 not found in PATH" >&2
        echo "       $2" >&2
        exit 1
    }
}
need meson "install from your package manager or pip install meson"
need ninja "install from your package manager"
need gcov  "ships with gcc — install build-essential / gcc"

# Prefer gcovr; fall back to lcov if missing. Both will be invoked
# at the end of this script; bail now if neither is present so the
# user doesn't sit through a full test run for nothing.
COVERAGE_FRONTEND=""
if command -v gcovr >/dev/null 2>&1; then
    COVERAGE_FRONTEND=gcovr
elif command -v lcov >/dev/null 2>&1 && command -v genhtml >/dev/null 2>&1; then
    COVERAGE_FRONTEND=lcov
else
    echo "error: neither gcovr nor lcov+genhtml is installed" >&2
    echo "       Fedora: dnf install gcovr" >&2
    echo "       Debian: apt install gcovr" >&2
    echo "       macOS:  brew install gcovr" >&2
    exit 1
fi

cd "$PROJECT_ROOT"

# Detect a stale build-cov/ dir whose recorded source path no
# longer matches this project root. This can happen if the dir
# was created in a different checkout, a chroot, a container, or
# a sandbox where the project was mounted at a different path —
# typical case is running an LLM-assisted setup that recorded a
# sandbox path, then trying to use the dir from the host. Meson
# `configure` accepts the options-only change but ninja later
# aborts with a confusing "Permission denied: /sessions" or
# similar when it tries to regenerate from the baked-in path.
#
# Two probes, cheapest first:
#   1. meson-info/meson-info.json — refreshed by `meson configure`
#      so this catches drift between options-update runs.
#   2. build.ninja — the regen path is baked in here at setup
#      time and is what ninja actually uses; this catches the case
#      where meson-info has been refreshed but build.ninja hasn't.
#
# If either disagrees with $PROJECT_ROOT, force a clean reset.
if [[ -d "$BUILD_DIR" ]] && [[ "$RESET" -eq 0 ]]; then
    stale_reason=""

    if [[ -f "$BUILD_DIR/meson-info/meson-info.json" ]]; then
        recorded_src=$(grep -o '"source"[[:space:]]*:[[:space:]]*"[^"]*"' \
            "$BUILD_DIR/meson-info/meson-info.json" 2>/dev/null \
            | head -n1 \
            | sed 's/.*"source"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/')
        if [[ -n "$recorded_src" ]] && [[ "$recorded_src" != "$PROJECT_ROOT" ]]; then
            stale_reason="meson-info source dir $recorded_src"
        fi
    fi

    if [[ -z "$stale_reason" ]] && [[ -f "$BUILD_DIR/build.ninja" ]]; then
        # The regenerate command embedded in build.ninja looks like:
        #   command = /usr/bin/meson --internal regenerate /SRC /BUILD
        # Pull out the SRC arg and compare. Falls back to silently
        # accepting if the line isn't found (don't be too clever).
        ninja_src=$(grep -m1 -oE '\-\-internal regenerate [^ ]+ ' \
            "$BUILD_DIR/build.ninja" 2>/dev/null \
            | awk '{print $3}')
        if [[ -n "$ninja_src" ]] && [[ "$ninja_src" != "$PROJECT_ROOT" ]]; then
            stale_reason="build.ninja regen source dir $ninja_src"
        fi
    fi

    if [[ -n "$stale_reason" ]]; then
        echo "==> $BUILD_DIR is stale ($stale_reason)"
        echo "    project root is $PROJECT_ROOT — wiping and reconfiguring"
        RESET=1
    fi
fi

# (Re)configure if needed.
if [[ "$RESET" -eq 1 ]] || [[ ! -d "$BUILD_DIR" ]]; then
    rm -rf "$BUILD_DIR"
    echo "==> meson setup $BUILD_DIR -Db_coverage=true"
    meson setup "$BUILD_DIR" -Db_coverage=true
else
    # Already configured — make sure b_coverage is on. Cheap no-op
    # if it already was.
    echo "==> meson configure $BUILD_DIR -Db_coverage=true"
    meson configure "$BUILD_DIR" -Db_coverage=true
fi

echo "==> meson compile -C $BUILD_DIR"
meson compile -C "$BUILD_DIR"

# Run tests. Suites control tier scope: unit + proto are always on;
# integration adds Tier 3 (which needs the Docker matrix up — tests
# skip cleanly when servers are unreachable, so this is safe to run
# either way).
SUITES=("unit" "proto")
if [[ "$QUICK" -eq 0 ]]; then
    SUITES+=("integration")
    # Light touch: warn if neither test server seems reachable on
    # the conventional ports, so the user doesn't get a 30 %-skipped
    # Tier 3 report and wonder why.
    if ! { (echo > /dev/tcp/127.0.0.1/5500) 2>/dev/null || \
           (echo > /dev/tcp/127.0.0.1/5510) 2>/dev/null; }; then
        echo "==> WARNING: no test server detected on 127.0.0.1:5500 (mhxd)"
        echo "    or 127.0.0.1:5510 (Janus). Tier 3 tests will skip."
        echo "    Bring up containers with the instructions in"
        echo "    tests/mhxd/README.md and tests/janus/README.md."
    fi
fi

SUITE_ARGS=()
for s in "${SUITES[@]}"; do
    SUITE_ARGS+=(--suite "$s")
done

echo "==> meson test -C $BUILD_DIR ${SUITE_ARGS[*]}"
# Don't abort on test failures — partial runs still produce useful
# coverage data, and the report itself is the point. The exit code
# from meson test is recorded so the script can echo it at the end.
TEST_RC=0
meson test -C "$BUILD_DIR" "${SUITE_ARGS[@]}" || TEST_RC=$?

# Generate the report.
rm -rf "$REPORT_DIR"
mkdir -p "$REPORT_DIR"

if [[ "$COVERAGE_FRONTEND" == "gcovr" ]]; then
    echo "==> gcovr → $REPORT_DIR/index.html"
    # gcovr reads .gcovr.cfg from the project root automatically
    # (cwd is set to PROJECT_ROOT above).
    gcovr \
        --root "$PROJECT_ROOT" \
        --object-directory "$BUILD_DIR" \
        --html-details "$REPORT_DIR/index.html" \
        --txt "$REPORT_DIR/summary.txt"
    echo
    echo "==> Top files by uncovered lines:"
    head -30 "$REPORT_DIR/summary.txt"
else
    echo "==> lcov + genhtml → $REPORT_DIR/index.html"
    LCOV_INFO="$REPORT_DIR/coverage.info"
    lcov --capture --directory "$BUILD_DIR" --output-file "$LCOV_INFO" \
         --rc lcov_branch_coverage=1 \
         --ignore-errors source,unused,empty \
         --quiet
    # Manual exclusions — lcov can't read .gcovr.cfg. Keep this
    # list in sync with the exclude= lines there.
    lcov --remove "$LCOV_INFO" \
         "*/src/xtext.[ch]" "*/src/dfa.[ch]" "*/src/rand.c" \
         "*/tests/*" "*/build*/*" "*/subprojects/*" \
         --output-file "$LCOV_INFO" \
         --rc lcov_branch_coverage=1 \
         --ignore-errors unused \
         --quiet
    genhtml "$LCOV_INFO" \
            --output-directory "$REPORT_DIR" \
            --branch-coverage \
            --ignore-errors source \
            --quiet
fi

echo
echo "==> Report ready: $REPORT_DIR/index.html"
if [[ "$TEST_RC" -ne 0 ]]; then
    echo "==> note: meson test exited $TEST_RC — some tests failed or were"
    echo "    skipped. Coverage data is still valid for the tests that ran."
fi

if [[ "$OPEN" -eq 1 ]]; then
    if command -v xdg-open >/dev/null 2>&1; then
        xdg-open "$REPORT_DIR/index.html" >/dev/null 2>&1 &
    elif command -v open >/dev/null 2>&1; then
        open "$REPORT_DIR/index.html"
    else
        echo "==> no xdg-open / open in PATH; open the file manually"
    fi
fi
