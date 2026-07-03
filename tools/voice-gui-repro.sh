#!/usr/bin/env bash
#
# voice-gui-repro.sh — reproduce the Janus "first joiner never hears the second
# joiner" bug with two REAL GtkHx GUI processes, headlessly.
#
# The VoiceRuntime test harness cannot trigger this bug (its programmatic timing
# doesn't hit the server-side race); only the full GtkHx GUI does. This script
# runs two real gtkhx instances under the GTK4 *Broadway* backend (a virtual
# display — no X/Wayland/GPU needed), with a virtual microphone so their
# autoaudiosrc send legs produce real RTP, and the GTKHX_VOICE_AUTOJOIN test
# hook driving each through the real Join-voice/unmute GUI path.
#
#   A (first joiner) starts, connects, joins voice, becomes the lone member.
#   B (second joiner) starts a few seconds later and joins.
#
# The bug's signature is an ASYMMETRY: B hears A (B gets a receive pad for A's
# mid) but A never hears B (no receive pad for B's mid). Asserting the
# asymmetry — rather than just "A got 0 audio" — makes the result robust: if
# NEITHER hears the other, that's an audio/setup problem, not the bug.
#
# Requires: gtk4-broadwayd, a PipeWire/pulse stack, and a reachable Janus-backed
# Hotline server. Env knobs:
#   GTKHX_VOICE_TEST_HOST / _PORT   server (default 127.0.0.1:5510)
#   STAGGER_S                        seconds A leads B (default 5)
#   RUN_S                            seconds to let them talk (default 16)
#   AUTOUNMUTE_MS                    ms after join before unmuting (default 3000)
#   OUTDIR                           logs (default /tmp/voice-gui-repro)
#   KEEP=1                           keep audio/broadway daemons running on exit

set -u

export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
mkdir -p "$XDG_RUNTIME_DIR" 2>/dev/null || true
chmod 700 "$XDG_RUNTIME_DIR" 2>/dev/null || true

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GTKHX="$ROOT/build/src/gtkhx"
OUTDIR="${OUTDIR:-/tmp/voice-gui-repro}"
HOST="${GTKHX_VOICE_TEST_HOST:-127.0.0.1}"
PORT="${GTKHX_VOICE_TEST_PORT:-5510}"
STAGGER_S="${STAGGER_S:-5}"
RUN_S="${RUN_S:-16}"
AUTOUNMUTE_MS="${AUTOUNMUTE_MS:-3000}"
mkdir -p "$OUTDIR"

PIDS=(); STARTED_STACK=0
log() { echo "[gui-repro] $*"; }
track() { PIDS+=("$1"); }
cleanup() {
  for p in "${PIDS[@]:-}"; do kill "$p" 2>/dev/null; done
  if [ "${KEEP:-0}" != "1" ] && [ "$STARTED_STACK" = "1" ]; then
    pkill -x wireplumber 2>/dev/null; pkill -x pipewire-pulse 2>/dev/null
    pkill -x pipewire 2>/dev/null
  fi
}
trap cleanup EXIT

[ -x "$GTKHX" ] || { log "ERROR: build the GUI first: meson compile -C build src/gtkhx"; exit 1; }
command -v gtk4-broadwayd >/dev/null || { log "ERROR: gtk4-broadwayd not found"; exit 1; }

# ---- audio: virtual mic carrying a real tone --------------------------------
if ! pactl info >/dev/null 2>&1; then
  log "starting PipeWire stack..."
  setsid pipewire       >/tmp/pw.log  2>&1 </dev/null & track $!
  setsid pipewire-pulse >/tmp/pwp.log 2>&1 </dev/null & track $!
  sleep 1
  setsid wireplumber    >/tmp/wp.log  2>&1 </dev/null & track $!
  STARTED_STACK=1
  for i in $(seq 1 20); do pactl info >/dev/null 2>&1 && break; sleep 0.5; done
fi
pactl info >/dev/null 2>&1 || { log "ERROR: audio server not reachable"; exit 1; }
pactl list short sinks 2>/dev/null | grep -q '\bvirtmic\b' || \
  pactl load-module module-null-sink sink_name=virtmic \
        sink_properties=device.description=virtmic >/dev/null
pactl set-default-source virtmic.monitor >/dev/null 2>&1
setsid gst-launch-1.0 -q audiotestsrc is-live=true wave=sine freq=440 volume=0.5 \
       ! audioconvert ! audioresample ! pulsesink device=virtmic sync=false \
       >/tmp/tone.log 2>&1 </dev/null & track $!
sleep 2
if timeout 6 gst-launch-1.0 -q pulsesrc num-buffers=25 ! audioconvert ! fakesink >/dev/null 2>&1; then
  log "virtual mic OK — real audio is flowing"
else
  log "WARNING: virtual mic not producing audio; result will be inconclusive"
fi

# ---- launch a headless gtkhx instance ---------------------------------------
# $1 label  $2 broadway-display-num  $3 nick  $4 logfile
launch_gui() {
  local label="$1" disp="$2" nick="$3" logf="$4"
  setsid gtk4-broadwayd ":$disp" >"/tmp/bwd$disp.log" 2>&1 </dev/null & track $!
  sleep 1
  log "launching $label (nick=$nick, broadway :$disp) -> $logf"
  USER="$nick" \
  GDK_BACKEND=broadway BROADWAY_DISPLAY=":$disp" \
  GTKHX_DEBUG=voice,voice-pipe,voice-flow \
  GTKHX_VOICE_AUTOJOIN=1 GTKHX_VOICE_AUTOUNMUTE_MS="$AUTOUNMUTE_MS" \
  GTKHX_VOICE_PUBLISH_DELAY_MS="${GTKHX_VOICE_PUBLISH_DELAY_MS:-0}" \
    setsid "$GTKHX" -s "$HOST" -t "$PORT" -l guest >"$logf" 2>&1 </dev/null & track $!
}

# ---- A joins first, becomes the lone member ---------------------------------
launch_gui "A (first joiner)"  2 GuiA "$OUTDIR/A.log"
log "waiting for A to connect + join voice (lone member)..."
for i in $(seq 1 30); do
  grep -qa "panel: state=4" "$OUTDIR/A.log" 2>/dev/null && { log "A is connected in voice."; break; }
  sleep 1
done
log "letting A settle for ${STAGGER_S}s before B joins..."
sleep "$STAGGER_S"

# ---- B joins second ---------------------------------------------------------
launch_gui "B (second joiner)" 3 GuiB "$OUTDIR/B.log"

log "running for ${RUN_S}s..."
sleep "$RUN_S"

# ---- assess the asymmetry ---------------------------------------------------
# uids each side sees: A's own uid appears in B's user-<uid> receive mid, etc.
a_send=$(grep -ac 'hxvoice-send-bin pay.src' "$OUTDIR/A.log" 2>/dev/null)
b_send=$(grep -ac 'hxvoice-send-bin pay.src' "$OUTDIR/B.log" 2>/dev/null)
a_recv=$(grep -ac 'receive bin LINKED'       "$OUTDIR/A.log" 2>/dev/null)
b_recv=$(grep -ac 'receive bin LINKED'       "$OUTDIR/B.log" 2>/dev/null)
a_padd=$(grep -aoE 'pad-added pad=[^ ]+ mid=user-[0-9]+' "$OUTDIR/A.log" 2>/dev/null | sort -u | tr '\n' ' ')
b_padd=$(grep -aoE 'pad-added pad=[^ ]+ mid=user-[0-9]+' "$OUTDIR/B.log" 2>/dev/null | sort -u | tr '\n' ' ')

echo "============================================================"
log "A (first joiner):  send-buffers=$a_send  recv-legs-linked=$a_recv  pads=[$a_padd]"
log "B (second joiner): send-buffers=$b_send  recv-legs-linked=$b_recv  pads=[$b_padd]"
echo "------------------------------------------------------------"
rc=2
if [ "$a_send" -gt 0 ] && [ "$b_send" -gt 0 ] && [ "$a_recv" -gt 0 ] && [ "$b_recv" -gt 0 ]; then
  log "RESULT: both peers hear each other — bug NOT reproduced (fixed / not triggered). PASS"
  rc=0
elif [ "$b_recv" -gt 0 ] && [ "$a_recv" -eq 0 ] && [ "$a_send" -gt 0 ]; then
  log "RESULT: *** BUG REPRODUCED *** — B hears A but A never hears B"
  log "        (A sent audio and saw B, but got no receive pad for B's mid)."
  rc=1
else
  log "RESULT: INCONCLUSIVE — neither peer sustained audio (audio/setup/server issue,"
  log "        not the asymmetry bug). Check $OUTDIR/*.log and the virtual mic."
  rc=3
fi
echo "============================================================"
log "logs: $OUTDIR/A.log  $OUTDIR/B.log"
exit "$rc"
