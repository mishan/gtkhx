#!/bin/sh
# Container entrypoint: launch argus (as the argus user) +
# stunnel in parallel, capture both stdout streams, and exit
# when either child dies. Argus is the long-running tracker
# daemon (binds 5698 plain); stunnel terminates TLS on 6498
# and forwards to argus.
#
# POSIX-shell only — debian:bookworm-slim's /bin/sh is dash,
# which doesn't have bash's `wait -n`. We poll kill -0 instead:
# strictly portable, and the latency cost (1s) is irrelevant
# given the container exists to run tests, not to handle
# real-time traffic.

set -eu

# Argus needs to run from its install dir to find config.yaml.
cd /opt/argus

# Drop to the argus user via su -c. stunnel runs as root because
# it needs to bind a privileged-ish port (6498 isn't actually
# privileged, but stunnel's default config style assumes root).
# The cert + key files are mode 0640 with group=argus so the
# argus user can read them too — not that we need that, but it
# keeps the file ownership story consistent.

su argus -s /bin/sh -c '/opt/argus/argus' &
ARGUS_PID=$!

# Give Argus a moment to bind 5698 before stunnel tries to
# connect-back. Without this, the first TLS client to land
# before Argus is ready gets a connect-refused that's hard to
# tell apart from a real failure.
sleep 1

stunnel /etc/stunnel/stunnel.conf &
STUNNEL_PID=$!

# Poll both PIDs. kill -0 sends no signal but errors if the
# pid no longer exists — the standard POSIX "is this process
# alive?" check. As soon as either dies, break out and reap
# the survivor. dash doesn't have `wait -n` so we can't block
# on "first child to exit"; this 1-second granularity is fine
# for a test container.
while kill -0 "$ARGUS_PID" 2>/dev/null && \
      kill -0 "$STUNNEL_PID" 2>/dev/null; do
    sleep 1
done

# One of them exited; bring the other down so the container
# unblocks cleanly.
kill "$ARGUS_PID" "$STUNNEL_PID" 2>/dev/null || true
wait
