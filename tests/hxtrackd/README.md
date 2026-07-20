# hxtrackd test tracker

A Docker container wrapping mhxd's `hxtrackd` — the canonical
pre-spec v1 Hotline tracker. Built from the same upstream mhxd
source as our `tests/mhxd/` container, but compiled and run as
hxtrackd instead of hxd.

## Why

GtkHx's tracker v3 work added a probe-then-fallback to the
network state machine: send the 8-byte v3 magic with a 2-second
read watchdog, fall back to a fresh 6-byte v1 magic on timeout.
The fallback path exists because real-world v1 trackers
(hltracker.com, hxd-family community trackers, mhxd's own
bundled hxtrackd) `memcmp` the full 6-byte `HTRK_MAGIC` against
`"HTRK\0\1"` and silently ignore connections whose version
byte is `0x03` instead of `0x01`. The spec's "v1 trackers read
6 bytes and respond" backcompat clause is wishful thinking
about pre-spec implementations.

We need a real v1-only tracker in the Tier 3 matrix so this
fallback can't regress silently. Argus (`tests/argus/`) covers
the v3 happy path; this container covers the v1 fallback path.

mhxd is the canonical-codebase reference server we already
vendor and trust — its bundled hxtrackd is the cleanest v1
implementation we have, and we already build it inside the
`tests/mhxd/` container (just don't expose it). Building it as
a separate first-class target here keeps the v1-fallback test
self-contained.

## Build

```sh
docker build -t gtkhx-hxtrackd tests/hxtrackd
```

The build clones mhxd master (same as `tests/mhxd/`), runs
`./autogen.sh && ./configure --enable-hxtrackd`, builds, installs
to `/opt/hxtrackd`, and overlays `tests/hxtrackd/conf/hxtrackd.conf`.

## Run

```sh
docker run --rm -p 5498:5498 -p 5499:5499/udp gtkhx-hxtrackd
```

hxtrackd uses its conventional `5498/5499` directly — these are
compile-time constants (`HTRK_TCPPORT`/`HTRK_UDPPORT`) that can't be
configured. In the multi-target Compose rig everything runs on host
networking, so Argus (whose ports *are* config-driven) is shifted up to
`5698/5699` to avoid the collision, leaving `5498/5499` free for
hxtrackd. The `hxtrackd` row in `tests/integration/tracker_matrix.c`
hard-codes `5498` as the listing port.

## Ports

| Port | Protocol | Purpose                                          |
|------|----------|--------------------------------------------------|
| 5498 | TCP      | Client listing requests (HTRK v1 only)           |
| 5499 | UDP      | Server registration heartbeats                   |

## What it serves

Every listing returns at least one deterministic entry, seeded
by `seed-tracker.py` running inside the container:

- **Name:** `hxtrackd test server`
- **Description:** `Tier 3 fixture — pinned by tests/hxtrackd`
- **Port:** 5500
- **Users:** 4
- **IP:** the container's bridge IP (Docker-assigned; not
  deterministic across container restarts, so tests assert on
  the other fields)

The seed script registers via UDP every minute so the entry
never expires. `hxtrackd.conf` bumps `tracker.interval` to
86400 (24 hours), so even a missed heartbeat won't drop the
entry within a single test run.

## Probe-then-fallback exercise

Connecting to this container exercises the entire v3-probe-fails
→ v1-retry path:

1. GtkHx sends 8-byte v3 magic (`HTRK\0\3` + 2 feature bytes).
2. hxtrackd reads 6 bytes, `memcmp`s against `HTRK\0\1`, fails,
   falls through. The connection stays open with no response.
3. GtkHx's 2-second read watchdog fires.
4. `tracker_fetch_retry_v1` closes the conn, opens a new one,
   sends the 6-byte v1 magic (`HTRK\0\1`).
5. hxtrackd accepts, echoes back the magic, streams the listing.
6. GtkHx parses v1 records and emits `HxTrackerServer` events
   for each.

`GTKHX_DEBUG=tracker ./build/src/gtkhx` traces every step.

## Iterate

To poke at hxtrackd from inside the container during a build
debug:

```sh
docker run --rm -it --entrypoint /bin/sh gtkhx-hxtrackd
```

Then `/opt/hxtrackd/run/bin/hxtrackd` runs it manually (note the
`run/` segment — mhxd's autotools override plants the binary inside
the build-tree `run/hxtrackd/` subdirectory; the Dockerfile copies
that whole subtree into `/opt/hxtrackd/run/`). The config files
live alongside the binary in `/opt/hxtrackd/run/`.

## Layout

```
tests/hxtrackd/
├── Dockerfile               build recipe (build mhxd, install
│                              hxtrackd, overlay conf, seed UDP)
├── conf/
│   └── hxtrackd.conf        replaces upstream
├── docker-entrypoint.sh     launches hxtrackd + seed loop
├── seed-tracker.py          UDP registration heartbeat
└── README.md                this file
```
