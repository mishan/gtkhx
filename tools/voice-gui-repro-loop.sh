#!/usr/bin/env bash
#
# voice-gui-repro-loop.sh — the first-joiner voice bug is a server-side RACE, so
# it's flaky: a single run often "works". Run voice-gui-repro.sh N times and
# tally REPRODUCED / PASS / INCONCLUSIVE. Exits non-zero if it reproduced at
# least once (which is all we need to call it a reliable repro of a flaky bug).
#
# Env: N (default 12), plus everything voice-gui-repro.sh honors
# (GTKHX_VOICE_TEST_HOST/_PORT, STAGGER_S, RUN_S, AUTOUNMUTE_MS, OUTDIR).
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
N="${N:-12}"
BASE_OUT="${OUTDIR:-/tmp/gui-repro-loop}"
mkdir -p "$BASE_OUT"
export KEEP=1                       # keep the audio stack up across iterations
export STAGGER_S="${STAGGER_S:-0}"  # B joins the instant A connects (tight race)
export RUN_S="${RUN_S:-9}"

repro=0; pass=0; inconc=0
for i in $(seq 1 "$N"); do
  echo "================= iteration $i/$N ================="
  OUTDIR="$BASE_OUT/iter$i" "$ROOT/tools/voice-gui-repro.sh" 2>&1 \
    | grep -E "RESULT|BUG REPRODUCED|send-buffers"
  rc=${PIPESTATUS[0]}
  case "$rc" in
    1) repro=$((repro+1));  echo ">>> iteration $i: REPRODUCED (A did not hear B)";;
    0) pass=$((pass+1));;
    *) inconc=$((inconc+1));;
  esac
  # let the server drop the just-closed participants before the next round
  sleep 3
done
echo "############################################################"
echo "SUMMARY over $N runs:  REPRODUCED=$repro  PASS=$pass  INCONCLUSIVE=$inconc"
echo "logs under $BASE_OUT/iterN/"
[ "$repro" -gt 0 ] && exit 1 || exit 0
