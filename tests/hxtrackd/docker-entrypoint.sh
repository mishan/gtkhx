#!/bin/sh
# Entrypoint for the hxtrackd test container.
#
# Layout:
#   1. Launch hxtrackd in the background (it listens on 5498/tcp +
#      5499/udp internally).
#   2. Spin up the seed-tracker.py heartbeat loop in the background
#      — sends an HTRK UDP registration every minute so the listing
#      always contains at least one deterministic entry.
#   3. wait on hxtrackd's PID so the container exits when the
#      tracker process dies (which never happens in steady state,
#      but lets `docker stop` propagate the signal cleanly).
#
# Logging goes to stdout/stderr via the seed script's print()
# calls and hxtrackd's hxd_log (which writes to stderr).
set -eu

cd /opt/hxtrackd/run

# Start hxtrackd. The binary lives at /opt/hxtrackd/run/bin/hxtrackd
# — NOT /opt/hxtrackd/bin/hxtrackd as the autotools --prefix would
# suggest. mhxd's src/hxtrackd/Makefile.am overrides exec_prefix
# to $(PWD)/../../run/hxtrackd, so `make install` plants the binary
# inside the build tree's run/hxtrackd/ subdirectory; the build
# stage's `cp -a /build/mhxd/run/hxtrackd /opt/hxtrackd/run` then
# carries it (along with the default hxtrackd.conf + tracker_banlist)
# into the runtime image. Same pattern src/hxd/ uses, which is why
# tests/mhxd/docker-entrypoint.sh similarly exec's
# /opt/mhxd/run/bin/hxd.
#
# It expects its config files in the working directory — hence
# the cd above (config.yaml + tracker_banlist are right there).
/opt/hxtrackd/run/bin/hxtrackd &
HXTRACKD_PID=$!

# Brief settle window so seed-tracker.py's first datagram lands
# after hxtrackd has bound 5499/udp. Without this, the first
# packet hits a not-yet-listening socket and gets silently dropped
# (UDP), and the test container would race during boot — a
# refresh issued in the first second would see an empty listing.
sleep 1

# Heartbeat loop in the background. The script loops itself, so a
# single launch is enough.
/usr/local/bin/seed-tracker.py &
SEED_PID=$!

# Trap SIGTERM/SIGINT so docker stop drains cleanly.
trap 'kill $HXTRACKD_PID $SEED_PID 2>/dev/null || true' INT TERM

# wait on the tracker; if it exits, take the whole container down.
wait $HXTRACKD_PID
