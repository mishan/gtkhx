# Argus test tracker

A Docker container wrapping VesperNet's [Argus](https://agora.vespernet.net/argus)
Hotline tracker server. We use it as our Tier 3 test target for the
**tracker v3 protocol** — the headline reason it exists in the
matrix — and as a regression net against future protocol-version
work.

## Why

GtkHx Phase A (claude/tracker-v3-phase-a) shipped support for the
v3 tracker protocol. We need a real v3 tracker to exercise the
production state machine end-to-end against actual wire bytes.
mhxd's bundled `hxtrackd` only speaks v1 (verified by reading the
source, see commit body for the tracker-v3-phase-a branch). Mock
v3 trackers would be busywork; Argus is the real thing.

Argus is the only public tracker we have access to that:

- Speaks v1, v2, AND v3 simultaneously on the same TCP port (5498)
  with automatic version detection.
- Ships a static binary that runs from a single config file with
  no external dependencies.
- Allows deterministic test content via the `promoted_servers`
  config section (no need for an actual Hotline server to register
  via UDP).

Source is closed. The Linux amd64 binary is publicly downloadable
from `get.vespernet.net`, and we build the container by pulling it
at image-build time + sha256-verifying. No redistribution — the
binary stays inside the test infrastructure on each developer's /
CI runner's machine. Same legal posture as Janus.

## Build

From the repo root:

```sh
docker build -t gtkhx-argus tests/argus
```

The build pulls
`https://get.vespernet.net/argus-linux-amd64.tar.gz` (~3.2 MB) and
verifies the sha256 against the value hard-coded in the Dockerfile.
A bump to a newer Argus build means swapping that `ARGUS_SHA256`
ARG; we deliberately don't auto-track upstream.

## Run

```sh
docker run --rm \
    -p 5498:5498 -p 6498:6498 -p 5499:5499/udp \
    gtkhx-argus
```

GtkHx points at the tracker by default (Settings → Tracker host),
so connecting is just opening the tracker window.

## Ports

| Port | Protocol  | Purpose                                                |
|------|-----------|--------------------------------------------------------|
| 5498 | TCP       | Client listing requests (v1/v2/v3 — auto-detected)     |
| 6498 | TCP + TLS | Phase D TLS-wrapped listing requests. stunnel sidecar terminates TLS here and forwards to plain Argus on `127.0.0.1:5498`. Self-signed cert generated at image-build time. |
| 5499 | UDP       | Server registration heartbeats (not driven by tests today) |

The TLS port is `6498` rather than the natural `5598` because
hxtrackd's container already publishes its plain endpoint on
host:5598. `+1000` is the next clean non-colliding step.

## What's enabled

Out of the box (`tests/argus/conf/config.yaml`):

- **v1 + v2 + v3 listing path** — Argus auto-detects the protocol
  version from the client's handshake. GtkHx Phase A sends an
  8-byte v3 handshake; Argus replies v3.
- **Three `promoted_servers` entries** — deterministic content for
  every listing fetch. Names "Promoted Alpha / Beta / Gamma",
  description text varies. See the gotcha below about why these
  arrive as hostname records, not IPv4 records.
- **`registration.reject_private_ips: false`** — Docker bridge IPs
  are RFC 1918; the default `true` would reject any UDP
  registration from inside the test rig. Not strictly needed for
  the current "client lists, no server registers" tests, but
  keeps the door open for a future test that registers a fake
  server inside the container.
- **`logging.level: debug`** — Argus prints one debug line per
  connection naming the negotiated v3 feature flags + the number
  of records served. Invaluable when Tier 3 fails mysteriously.

Not enabled:

- **API server, federation, Mnemosyne content enrichment, HMAC
  registration, account-based auth.** All disabled in upstream's
  default config too; we just don't flip them on.

## Known gotcha — promoted entries are hostname records

Argus 1.0.2 emits **all** `promoted_servers` entries as v3 `0x48`
(hostname) records on the wire, even when the YAML `address` is a
literal IP like `"203.0.113.10:5500"`. The spec allows it — a
hostname is just a UTF-8 string, and an IP literal is a valid
hostname.

The client-side parser (`src/tracker_v3.c::hx_tracker_v3_parse_record`)
handles all three address-type bytes (`0x04`/`0x06`/`0x48`)
cleanly, pinned by `tests/proto/test_tracker_v3.c::test_record_hostname`.

The view side (`src/tracker.c::tracker_server_create`) routes all
three through the same string-keyed dedup tree — added in the
Phase E rewrite that landed on `claude/tracker-v3-phase-a` —
so hostname records flow to the UI exactly like IPv4 records.
`hx_connect` takes a host string and `getaddrinfo` resolves
literal IPv4 / IPv6 / hostnames transparently.

The Tier 3 test still asserts at the **wire-parser layer** rather
than the boxed-event signal layer because the UI render path would
need a GMainLoop test harness this binary doesn't ship; the
production state machine processes the same byte sequence either
way.

## TLV coverage from `promoted_servers`

`promoted_servers` only takes `address` / `name` / `description`,
which means the TLV trailer Argus emits for these records is just
the tracker-injected 0x0600 block (`IS_PROMOTED`, optionally
`FIRST_SEEN` / `LAST_HEARTBEAT`). The richer descriptive +
capability + content-index TLV blocks (`0x0200` / `0x0300` /
`0x0400` / `0x0500`) would only appear if a real Hotline server
UDP-registered against this tracker and supplied them — Argus
doesn't fabricate them itself.

For Phase B, the typed-meta decoder (`hx_tracker_v3_meta_new` in
`src/tracker_v3_meta.c`) gets per-TLV coverage from synthetic
fixtures in `tests/proto/test_tracker_v3_meta.c`, and the Tier 3
test here cross-checks the 0x0600 block by asserting
`meta->is_promoted` for the Promoted Alpha record. A future
follow-up could wire a small UDP-registration helper (parallel to
`tests/hxtrackd/seed-tracker.py`) that registers a synthetic v3
server with a populated TLV trailer; that's deferred until we have
a concrete use case the existing Tier 2 fixtures don't already
cover.

## Connecting GtkHx

In the running app:

1. Settings → Tracker host → `localhost:5498` (or whichever port
   you mapped).
2. Open the tracker window (Toolbar → Tracker).
3. Click Refresh.

`GTKHX_DEBUG=tracker ./build/src/gtkhx` shows the v1/v3 fork
decision + Argus's feature flags in the terminal.

## Iterate

To test a different Argus build, edit the `ARGUS_URL` and
`ARGUS_SHA256` ARGs in the Dockerfile. If upstream churns the URL
shape, the curl will fail; if the bytes change, the sha256 check
will fail — both are explicit signals.

To experiment with a config tweak (extra promoted entries, enable
the API for poking around, etc.), edit `conf/config.yaml` and
rebuild. Upstream's `configuration.md` (also in the tarball) is
the authoritative reference.

## Layout

```
tests/argus/
├── Dockerfile           build recipe (curl + sha256 + COPY)
├── conf/
│   └── config.yaml      full config (replaces upstream)
└── README.md            this file
```

`conf/config.yaml` is checked in whole rather than `sed`-applied in
the Dockerfile so the diff against upstream is auditable and robust
to upstream re-wording. Same pattern as the Janus / mhxd
containers.
