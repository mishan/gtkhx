#!/bin/sh
# chatbench.sh — run the chat-view A/B benchmark against both backends.
#
# Needs a display: this drives the real widget through the real frame
# clock, which is the only way to measure what a user actually feels.
#
#   tools/chatbench.sh [messages] [repeats]
#
# Defaults to 20000 messages, 3 repeats per backend. Repeats matter more
# than they look — frame timings on a live compositor are noisy, and one
# run of each is not enough to tell a real difference from scheduler
# luck. Read the spread, not the single number.
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

echo "chatbench: $N messages, $REPEATS repeats per backend, $BIN"

# Which backend each pass *asked* for, and which it reported using.
# These are compared at the end: the first version of this script passed
# GTKHX_CHATVIEW=0/1, but the selector only understood "new"/"hxchat", so
# both passes silently ran xtext and produced a full set of plausible
# numbers that meant nothing. Never again without the script noticing.
seen_xtext=""
seen_hxchat=""

run_one () {
    _sel="$1"
    _label="$2"
    _i="$3"
    echo
    echo "--- $_label run $_i/$REPEATS (GTKHX_CHATVIEW=$_sel) ---"
    _out=$(GTKHX_CHATVIEW="$_sel" \
           GTKHX_CHATVIEW_BENCH="$N" \
           GTKHX_CHATVIEW_BENCH_QUIT=1 \
               "$BIN" 2>/dev/null | sed -n '/=== chat-view benchmark/,/^====/p')
    echo "$_out"
    case "$_out" in
        *"hxchat (new)"*) seen_hxchat="y" ;;
        *"xtext (old)"*)  seen_xtext="y" ;;
    esac
}

i=1
while [ "$i" -le "$REPEATS" ]; do
    run_one xtext "xtext (old)" "$i"
    i=$((i + 1))
done

i=1
while [ "$i" -le "$REPEATS" ]; do
    run_one new "hxchat (new)" "$i"
    i=$((i + 1))
done

echo
if [ -z "$seen_xtext" ] || [ -z "$seen_hxchat" ]; then
    echo "chatbench: ERROR — did not get a run from both backends." >&2
    echo "  xtext seen: ${seen_xtext:-no}   hxchat seen: ${seen_hxchat:-no}" >&2
    echo "  The numbers above are NOT an A/B comparison. Discard them." >&2
    exit 1
fi

echo "Done. Compare 'ingest + paint', 'reflow (width)' and the scroll"
echo "percentiles across the two backends; ingest alone is not comparable"
echo "(the backends defer different amounts of work — see chat_bench.c)."
