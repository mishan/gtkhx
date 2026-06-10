# Janus test server

A Docker container wrapping VesperNet's [Janus](https://agora.vespernet.net/janus)
Hotline server. We use it as our Tier 3 test target for the
chat-history protocol extension (the headline reason Janus exists in
the matrix) and as a general "modern, feature-rich server" target
alongside the older mhxd we already wrap.

## Why

mhxd (`tests/mhxd/`) is the canonical-codebase reference server —
same family GtkHx's protocol layer descends from, so it's the
controlled target for base-protocol round-trips. But it doesn't ship
chat-history, doesn't ship TLS, and lags well behind on modern
extensions.

Janus is the only public server we've found that:

- Implements the fogWraith chat-history extension (SQLite-backed,
  cursor pagination).
- Implements native TLS on a separate port.
- Implements all of HOPE / large files / text encoding / capability
  negotiation as a coherent set.

Source is closed. The Linux amd64 binary is publicly downloadable
from `get.vespernet.net`, and we build the container by pulling it
at image-build time + sha256-verifying. No redistribution — the
binary stays inside the test infrastructure on each developer's /
CI runner's machine.

## Build

From the repo root:

```sh
docker build -t gtkhx-janus tests/janus
```

The build pulls
`https://get.vespernet.net/janus-linux-amd64.tar.gz` (~9.5 MB) and
verifies the sha256 against the value hard-coded in the Dockerfile.
A bump to a newer Janus build means swapping that `JANUS_SHA256`
ARG; we deliberately don't auto-track upstream.

Container build also seeds a few deterministic files into the
`Files/` tree and enables `ChatHistoryEnabled` in the upstream
config.

## Run

```sh
docker run --rm -p 5510:5500 -p 5511:5501 \
                -p 5514:5504/udp \
                -p 5610:5600 -p 5611:5601 \
                gtkhx-janus
```

Host ports keep mhxd's conventional 5500/5501 free for side-by-side
use via the multi-server Compose setup, applying a uniform +10
offset: 5510/5511 for plaintext TCP, 5514 for voice's base+4 UDP,
5610/5611 for TLS (control + HTXF subchannel). Connect with:

```
Server:  localhost:5510
Login:   guest
Pass:    (empty)        (upstream default — empty password works
                         for HOPE login too; Janus computes
                         HMAC(key="", session_key) server-side)
```

Or as admin (full access):

```
Login:   admin
Pass:    adminpass      (set by the Dockerfile HOPE-seed step)
```

Janus's default `guest` account has `ReadChatHistory: true` already
set (access bit 56), so chat-history queries from a guest connection
work without any tweak.

## Ports

| Port | Protocol | Purpose                                                |
|------|----------|--------------------------------------------------------|
| 5500 | TCP      | HTLS — main client connection                          |
| 5501 | TCP      | HTXF — file transfer subchannel                        |
| 5504 | UDP      | WebRTC voice (ICE/DTLS/RTP, base+4)                    |
| 5600 | TCP      | HTLS over TLS — self-signed cert generated at build    |
| 5601 | TCP      | HTXF over TLS                                          |

## What's enabled

Out of the box:

- Full Hotline protocol (chat, PM, news, files, agreement, banner).
- **Chat history extension** (`ChatHistoryEnabled: true`,
  SQLite-backed, default retention = unlimited).
- **HOPE-Secure-Login + ChaCha20-Poly1305 AEAD**
  (`EnableHOPE: true`). Two paths land at a server that
  accepts HOPE login:
    1. *Empty password* (guest's upstream default). bcrypt-of-
       empty in the YAML, no `HOPEPassword:` field. Janus's
       HOPE login handler computes `HMAC(key="", session_key)`
       server-side and compares. Works out of the box —
       `test_hope_chacha20` uses this path, matching
       hotline.vespernet.net's guest configuration.
    2. *Non-empty password* via the admin REST API. The
       Dockerfile exposes the API on `:8973` and PATCHes
       `admin` to `"adminpass"`. Janus writes a `HOPEPassword:`
       blob into `Server/Users/admin.yaml`, but empirically
       the blob doesn't validate at HOPE login (likely a Janus
       issue). We seed it anyway in case future tests need
       a non-empty HOPE password and the path gets fixed.
  The master key Janus generated for the encryption sits in
  `Server/Data/` and ships in the image.
- Large-file (>4 GiB) transfers.
- Text encoding negotiation (UTF-8 / Mac Roman).
- File-mode banner (Janus ships a `banner.gif`).
- Threaded news (Hotline 1.5+).
- **Voice chat extension** (`EnableVoice: true`, fogWraith
  Capabilities-Voice.md). `VoiceUDPPort: 5504` is pinned
  explicitly (base+4). `NewUserDefaults.VoiceChat: true` gives
  any runtime-created account access bit 55 by default; the
  bundled `guest` and `admin` accounts get the bit through an
  in-place YAML edit in `seed-hope-passwords.sh` (the upstream
  YAML schema is one boolean per access bit, two-space-indented
  under `Access:`).

  GtkHx Phase 8.A advertises `HTLC_CAP_VOICE` in LOGIN; Janus
  with this config echoes the cap and the client sees voice as
  available. No media flows in Phase 8.A — the WebRTC pipeline
  and audio I/O land in Phases 8.B and 8.C. The 5504/udp port
  mapping is therefore inert today and only becomes load-bearing
  with the runtime crate.

Also enabled:

- **TLS** on 5600 (control) and 5601 (HTXF subchannel). The
  Dockerfile generates a self-signed cert (CN=localhost,
  SAN=DNS:localhost,IP:127.0.0.1, 10-year validity, 2048-bit RSA)
  into `Server/tls/` before the seed step (the seed-time Janus
  process refuses to start without it). Janus is the canonical TLS
  test target — `test_integration_real_tls` and the Tier 3 TLS
  matrix rows depend on this. The Phase 1 client trust path
  accepts any cert via an accept-certificate stub; the Phase 3
  trust UI lands the actual pinning flow.

Not enabled (out of scope for GtkHx):

- **IRC bridge / NewsBridge / Mnemosyne content sync.**

## Iterate

To test a different Janus build, edit the `JANUS_URL` and
`JANUS_SHA256` ARGs in the Dockerfile. If upstream churns the URL
shape, the curl will fail; if the bytes change, the sha256 check
will fail — both are explicit signals.

To experiment with a config tweak (HOPE on/off, retention settings,
etc.), edit the `sed` step or add a new one. The upstream config is
~750 lines of well-commented YAML; spending a couple of minutes
reading the relevant section is faster than guessing.

## Connecting GtkHx

In the running app:

1. Toolbar → Connect (or Ctrl+K).
2. Server: `localhost`, Port: `5510`.
3. Login / Password as above.
4. Click Connect.

`GTKHX_DEBUG=proto ./build/src/gtkhx` shows the wire conversation
in the terminal — particularly useful here for diagnosing the
chat-history TRAN 700 round-trip once that lands in GtkHx.

## Known gotchas

**Per-IP connect-rate limit.** Janus refuses rapid reconnects from
the same IP with `"Rate limit exceeded"` in its log. Verified by
running the existing `handshake` integration test against a
locally-launched Janus — the first connection succeeds the magic
handshake, the second comes back almost immediately and Janus
rejects it before it can read the magic bytes.

The Tier 3 integration suite fires several connections per second
from 127.0.0.1, so this will bite us once we wire any of the
existing tests against the `janus` matrix entry. We don't see this
on mhxd because we disabled its `nospam` flag explicitly in the
config; Janus's limiter doesn't appear to have a user-facing
on/off in `config.yaml`. Three possible fixes, none implemented
yet:

1. Spread the test connections out in time (sleep 50-100 ms
   between connects in the harness's connect helper).
2. Find the limiter knob in Janus — it may exist deeper in the
   config or only via the REST API. The strings in the binary
   suggest the limit is hard-coded with a short cooldown; worth
   confirming with VesperNet rather than reverse-engineering.
3. Bind Janus's listener to a different test-client IP per test
   so each test has its own per-IP counter (overkill).

Phase D — wiring chat-history tests against this container — will
need to pick one of these. For now, manual one-off connects work
fine and the container boots cleanly.

## Layout

```
tests/janus/
├── Dockerfile               build recipe (curl + sha256 + copy + seed)
├── conf/
│   └── config.yaml          full server config (replaces upstream)
├── seed-hope-passwords.sh   build-time HOPE password seeder
└── README.md                this file
```

`conf/config.yaml` is the upstream Janus 2.0.8-dev `config.yaml`
with the test-targeted edits baked in (HOPE on, chat-history on,
per-IP throttles disabled, admin API exposed for the build-time
HOPE seeding). It's checked in whole rather than `sed`-applied
in the Dockerfile so the diff against upstream is auditable and
robust to upstream re-wording. Mirrors how `tests/mhxd/conf/`
ships its full `hxd.conf`.

`seed-hope-passwords.sh` is a standalone script the Dockerfile
invokes during the build stage. It starts Janus once, PATCHes
the bundled guest/admin passwords via the admin REST API to
generate HOPE-compatible hashes, then shuts Janus down. Lives
in its own file because Dockerfile `RUN` steps mixing `&`
(background) with `&&` (conditional) silently misparse; a
standalone `set -euo pipefail` script keeps the control flow
auditable.
