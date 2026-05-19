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
docker run --rm -p 5510:5500 -p 5511:5501 gtkhx-janus
```

Host ports 5510/5511 keep mhxd's conventional 5500/5501 free for
side-by-side use via the multi-server Compose setup. Connect with:

```
Server:  localhost:5510
Login:   guest
Pass:    (empty)
```

Janus's default `guest` account has `ReadChatHistory: true` already
set (access bit 56), so chat-history queries from a guest connection
work without any tweak.

## Ports

| Port | Protocol | Purpose                                                |
|------|----------|--------------------------------------------------------|
| 5500 | TCP      | HTLS — main client connection                          |
| 5501 | TCP      | HTXF — file transfer subchannel                        |
| 5600 | TCP      | HTLS over TLS (reserved; TLS not enabled in v1)        |
| 5601 | TCP      | HTXF over TLS (reserved)                               |

## What's enabled

Out of the box:

- Full Hotline protocol (chat, PM, news, files, agreement, banner).
- **Chat history extension** (`ChatHistoryEnabled: true`,
  SQLite-backed, default retention = unlimited).
- Large-file (>4 GiB) transfers.
- Text encoding negotiation (UTF-8 / Mac Roman).
- File-mode banner (Janus ships a `banner.gif`).
- Threaded news (Hotline 1.5+).

Not enabled in v1 (separate follow-ups):

- **HOPE secure login.** Janus's HOPE path requires re-setting
  account passwords so the server can derive the HOPE-compatible
  hash. The upstream guest/admin yaml files don't ship with such
  hashes. Worth enabling once we have a clear story for seeding
  HOPE-friendly accounts at image-build time.
- **TLS.** `Extras/generate_cert.sh` + `Extras/openssl.cnf` would
  generate a self-signed cert at build time; we haven't audited
  that flow yet. Pencilled in alongside the GtkHx-side TLS work.
- **Voice chat / IRC bridge / NewsBridge / Mnemosyne content
  sync.** Out of scope for GtkHx.

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

**TLS / HOPE not enabled yet.** See "What's enabled" above. These
are deliberate v1 omissions, not bugs.

## Layout

```
tests/janus/
├── Dockerfile   build recipe (curl + sha256 + sed + seed)
└── README.md    this file
```

No checked-in config tree (yet) — the Dockerfile relies on the
upstream config.yaml from the pinned tarball, with one `sed` line
for the chat-history toggle. If we end up needing more than a few
config tweaks we can extract a `conf/` directory the same way
`tests/mhxd/conf/` is structured.
