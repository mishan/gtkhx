#!/bin/sh
# chatbench.sh — chat-view performance baseline.
#
# Needs a display: this drives the real widget through the real frame
# clock, which is the only way to measure what a user actually feels.
#
#   tools/chatbench.sh [messages] [repeats]
#
# Defaults to 20000 messages, 3 repeats. Repeats matter more than they
# look — frame timings on a live compositor are noisy, and one run is not
# enough to tell a real difference from scheduler luck. Read the spread,
# not the single number.
#
# This was an A/B against xtext until C5 deleted it. The comparison is in
# docs/chat-view-benchmark.md and cannot be re-run. What remains is a
# regression baseline: it can tell you the chat view got slower, not that
# it was ever better than what it replaced.
#
# See src/chat_bench.c for what each metric measures and what it doesn't.

set -eu

N="${1:-20000}"
REPEATS="${2:-3}"
BIN="${GTKHX_BIN:-./build/src/gtkhx}"

if [ ! -x "$BIN" ]; then
    echo "chatbench: no binary at $BIN" >&2
    echo "  build first (meson compile -C build), or set GTKHX_BIN" >&2
    exit 1
fi

if [ -z "${DISPLAY:-}" ] && [ -z "${WAYLAND_DISPLAY:-}" ]; then
    echo "chatbench: no display — this benchmark drives a real frame clock" >&2
    exit 1
fi

echo "chatbench: $N messages, $REPEATS repeats, $BIN"

i=1
while [ "$i" -le "$REPEATS" ]; do
    echo
    echo "--- run $i/$REPEATS ---"
    GTKHX_CHATVIEW_BENCH="$N" \
    GTKHX_CHATVIEW_BENCH_QUIT=1 \
        "$BIN" 2>/dev/null | sed -n '/=== chat-view benchmark/,/^====/p'
    i=$((i + 1))
done

echo
echo "Done. 'ingest + paint', 'relayout worst frm' and the scroll"
echo "percentiles are the ones to watch for regressions. Reference values"
echo "from the C5 A/B run are in docs/chat-view-benchmark.md."
