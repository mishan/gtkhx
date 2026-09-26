#!/bin/sh
# uibench.sh — in-app UI benchmarks through the real frame clock.
#
#   tools/uibench.sh [scenarios] [repeats]
#
# `scenarios` is the GTKHX_BENCH list, default "chat=20000,files=10000".
# Repeats default to 3 and matter more than they look: frame timings on a
# live compositor are noisy, and one run can't tell a real difference from
# scheduler luck. Read the spread, not the single number.
#
# Needs a display. Run it on the desktop for numbers worth recording; under
# tools/isolated-run.sh (Xvfb) it works, but the idle frame interval is
# Xvfb's, not a display's.
#
# Every report leads with the idle frame interval, the known value the rest
# is checked against. See rust/crates/gtkhx-ui/src/bench/mod.rs for what
# each scenario measures, and docs/performance.md for recorded baselines.

set -eu

SCENARIOS="${1:-chat=20000,files=10000}"
REPEATS="${2:-3}"
BIN="${GTKHX_BIN:-./build/src/gtkhx}"

if [ ! -x "$BIN" ]; then
    echo "uibench: no binary at $BIN" >&2
    echo "  build first (meson compile -C build), or set GTKHX_BIN" >&2
    exit 1
fi

if [ -z "${DISPLAY:-}" ] && [ -z "${WAYLAND_DISPLAY:-}" ]; then
    echo "uibench: no display — these benchmarks drive a real frame clock" >&2
    exit 1
fi

echo "uibench: $SCENARIOS, $REPEATS repeats, $BIN"

# GTK's own stderr chatter is noise here, but when the app fails (a bad
# scenario list, a crash) its stderr is the only explanation.
errlog=$(mktemp)
trap 'rm -f "$errlog"' EXIT

i=1
while [ "$i" -le "$REPEATS" ]; do
    echo
    echo "--- run $i/$REPEATS ---"
    rc=0
    GTKHX_BENCH="$SCENARIOS" GTKHX_BENCH_QUIT=1 \
        "$BIN" 2>"$errlog" >"$errlog.out" || rc=$?
    sed -n '/^=== /,/^====/p' "$errlog.out"
    rm -f "$errlog.out"
    if [ "$rc" -ne 0 ]; then
        echo "uibench: $BIN exited with status $rc:" >&2
        tail -n 20 "$errlog" >&2
        exit "$rc"
    fi
    i=$((i + 1))
done
