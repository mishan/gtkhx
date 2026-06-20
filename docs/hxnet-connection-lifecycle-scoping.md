# hxnet Connection-Lifecycle Ownership — Scoping Notes

Status: scoping, not yet started. Written 2026-06-20 against the
state of `main` after R3.3.e shipped (hxnet handles the post-HOPE
frame I/O) and the `claude/r3.3e-tls-hxnet` branch (hxnet can
TLS-wrap a TCP socket).

The R3.3.e family solved hxnet doing the *easy* half of the
Hotline lifecycle — post-login frame traffic, where the wire shape
is uniform 22-byte headers and the cipher state is already
negotiated. The hard half — TCP connect, TLS handshake, magic
exchange, LOGIN send, HOPE handshake, cipher negotiation,
AGREEMENT exchange — still lives in C across `network.c` (~3,800
LOC), `rcv.c` (~3,000 LOC), and `cipher.c` (~380 LOC). This doc
scopes moving the rest of the connect-time lifecycle into hxnet
so the C side eventually only sees post-handshake frames.

**The bigger reward:** R3.3.e-4f (legacy GIOStream cleanup) can
actually delete the `control_*` helpers, the GPollable read/write
loop, and the per-call `cipher_encode` / `compress_encode`
splices — none of which are reachable any more once hxnet owns the
whole pre-frame window. The current model can only prune the
*post-install* parts of those paths, which is a small fraction of
the surface area.

---

## 1. What we're displacing

Three C-side responsibilities move into Rust. Their current
implementations are interleaved across files, but the
responsibilities are individually clear:

### 1.1 TCP / TLS connect (`network.c`, ~250 LOC)

- `hx_connect` (line 1897) — entry point. Builds a
  `gtkhx_connect_ctx`, creates a `GSocketClient`, optionally
  flips `g_socket_client_set_tls(TRUE)`, kicks off
  `g_socket_client_connect_to_host_async`.
- `on_async_connected` — async callback after TCP succeeds (and
  TLS handshake completes, if `tls`). Stashes the
  `GSocketConnection` on `current_conn`, starts the magic-write.
- `on_socket_client_event` (line 1879) — TLS handshaking signal
  handler. Attaches `tls_accept_certificate` for cert validation.
- `tls_accept_certificate` (line 1762) — fingerprint lookup,
  cross-port silent-accept, GTKHX_TLS_AUTO_ACCEPT escape hatch,
  dialog marshalling.

### 1.2 Magic exchange (`network.c`, ~80 LOC)

- `g_output_stream_write_all_async (HTLC_MAGIC, ...)` at line
  1399. Asynchronous magic write.
- `on_magic_sent` → reads HTLS_MAGIC via
  `g_input_stream_read_all_async`. State transition to
  `GTKHX_CONNECTION_MAGIC_DONE`.
- `populate_htlc_remote_ip`, qbuf init, install of
  `control_arm_read_source` — all incidental cleanup.

### 1.3 LOGIN send + HOPE handshake (`network.c` + `rcv.c` +
`cipher.c`, ~1200 LOC)

- `send_login` (network.c line 1414) — 200 LOC. Two paths:
  - **Plaintext** — LOGIN opcode with login + password chunks
    only.
  - **HOPE** — LOGIN opcode with login (plaintext placeholder),
    HOPE_APP_ID, HOPE_APP_STRING, SESSIONKEY (empty), MAC_ALG
    list, CIPHER_ALG list, COMPRESS_ALG list (when applicable).
- `rcv_task_login` (rcv.c line 1734) — 600 LOC. State machine
  that handles the server's HOPE response across two TASK replies:
  - HOPE STEP1 reply — server's MAC choice + session key seed.
  - HOPE STEP2 reply — server's cipher / compress choice, derives
    keys via `hope_step2_derive`, calls `cipher_*_init` /
    `compress_*_init`.
- `cipher_aead_derive_session_keys` (cipher_aead.c) — HKDF-SHA256
  key derivation for ChaCha20-Poly1305.
- `cipher_change_decode_key` / `cipher_change_encode_key`
  (cipher.c) — Blowfish rekey marker handling. **Note:** this is
  separate from the legacy HOPE handshake — it's a per-message
  rotation that fires inside the post-login frame stream. Already
  implemented in `hxnet::hope_blowfish::HopeBlowfishStream`; the
  C version stays for the legacy GIOStream path until R3.3.e-4f
  retires it.

### 1.4 AGREEMENT exchange (`rcv.c`)

- Server sends `HTLS_HDR_AGREEMENT` after HOPE completes (or
  immediately after LOGIN reply in plaintext mode).
- C side displays the agreement text in a dialog; user clicks
  Agree.
- Client sends `HTLC_HDR_AGREEMENTAGREE`.

This step is **already a frame on the wire**, so it doesn't have
to move at the same time as 1.1–1.3. We can have hxnet emit the
AGREEMENT frame to C and let C continue to drive the dialog +
AGREEMENTAGREE send, exactly the same way it would for any other
post-handshake frame. The only catch is that the bridge has to
already be installed in Frame mode by then.

---

## 2. The new model: hxnet owns the whole pre-frame window

Production callers (Connect dialog, bookmarks dialog, reconnect)
hand hxnet a single config struct and listen for connection-state
events. hxnet drives every step from DNS resolution through to
the AGREEMENT frame, then transitions to its existing frame I/O
mode for the rest of the connection.

### 2.1 Connection lifecycle state machine

```text
              ┌─────────┐
              │  Idle   │
              └────┬────┘
                   │  open(config)
                   ▼
       ┌─────────────────────┐
       │     Resolving       │   DNS / SOCKS / GProxyResolver
       └─────────┬───────────┘
                 ▼
       ┌─────────────────────┐
       │      Connecting     │   TCP SYN sent, waiting for ACK
       └─────────┬───────────┘
                 ▼              (tls only)
       ┌─────────────────────┐
       │   TlsHandshaking    │   tokio-rustls handshake +
       │                     │   accept-cert callback
       └─────────┬───────────┘
                 ▼
       ┌─────────────────────┐
       │     MagicExchange   │   Write HTLC_MAGIC; read HTLS_MAGIC
       └─────────┬───────────┘
                 ▼
       ┌─────────────────────┐
       │     LoginSending    │   Send LOGIN opcode (chunks per HOPE/plain)
       └─────────┬───────────┘
                 ▼
       ┌─────────────────────┐
       │   LoginReplyWait    │   Receive TASK reply
       └─────────┬───────────┘
                 ▼              (hope only — 2-step path)
       ┌─────────────────────┐
       │    HopeStep1        │   Parse server MAC choice + sessionkey
       └─────────┬───────────┘
                 ▼
       ┌─────────────────────┐
       │    HopeStep2        │   Build step2 LOGIN, send, await reply
       └─────────┬───────────┘
                 ▼
       ┌─────────────────────┐
       │  CipherTransition   │   Apply negotiated cipher to transport
       └─────────┬───────────┘
                 ▼              (all paths converge)
       ┌─────────────────────┐
       │      Frame Mode     │   Connection actor reads frames,
       │                     │   emits Event::Frame to C
       └─────────────────────┘
```

Failures at any step exit to `Shutdown(reason)` with a specific
reason variant (HostNotFound, ConnectionRefused, TlsHandshakeFailed,
MagicMismatch, LoginRejected, HopeNegotiationFailed, ...) so the C
side can surface meaningful errors.

### 2.2 Event taxonomy

Existing events stay: `Event::Frame(Frame)`,
`Event::Shutdown(ShutdownReason)`.

New events for connection-state progress (each fires once, in the
order shown above):

```rust
enum Event {
    State(ConnectionState),  // every transition
    Frame(Frame),            // post-handshake traffic (unchanged)
    Shutdown(ShutdownReason),// final
}

enum ConnectionState {
    Resolving,
    Connecting,
    TlsHandshaking,
    MagicExchange,
    LoginSending,
    LoginReplyWait,
    HopeStep1,
    HopeStep2,
    CipherTransition,
    HandshakeDone,           // entering Frame Mode
}
```

The C side maps these onto the existing
`gtkhx_session_emit_connection_state` signals, which the toolbar
and chat windows already listen to for the throbber / status
text. No new GtkhxSession signals — just more granular reporting
via the existing one.

### 2.3 New FFI surface

```rust
pub struct HxnetConnectionConfig {
    pub host: *const u8,        // SNI / DNS / IP literal
    pub host_len: usize,
    pub port: u16,
    pub tls: bool,
    pub login: *const u8,       // utf-8, non-NUL-terminated
    pub login_len: usize,
    pub password: *const u8,
    pub password_len: usize,
    pub icon_name: *const u8,
    pub icon_name_len: usize,
    pub client_version: u16,
    pub use_hope: bool,         // whether to attempt HOPE-Secure
    pub mac_alg_prefs: *const u8,    // serialised list: "HMAC-SHA256\0HMAC-SHA1\0HMAC-MD5\0\0"
    pub cipher_alg_prefs: *const u8, // same shape
    pub compress_alg_prefs: *const u8, // same shape
    pub on_verify_cert: HxnetTlsTrustCallback,  // tls only
    pub on_verify_cert_user_data: *mut c_void,
}

#[no_mangle]
pub unsafe extern "C" fn hxnet_connection_open(
    config: *const HxnetConnectionConfig,
    on_event: HxnetEventCallback,
    on_shutdown: HxnetShutdownCallback,
    user_data: *mut c_void,
) -> *mut HxnetConnection;
```

The existing `spawn_fd_*` family stays for tests that want to
adopt a pre-connected socket without driving the full lifecycle.

---

## 3. Phasing

Five phases, each landable. Order matters — each phase depends on
the prior one being on `main`.

### Phase A — TCP connect inside hxnet
`~150 LOC src + ~150 LOC tests, 1-2 days`

- `tokio::net::TcpStream::connect` with `tokio::net::lookup_host`
  for DNS. Matches `GSocketClient`'s try-each-resolved-address
  fallback shape.
- Optional `GProxyResolver`-equivalent — `tokio` doesn't have a
  built-in SOCKS resolver; if proxy support matters, pull in
  `tokio-socks` (MIT, small). The current C path picks SOCKS up
  for free via GIO; Phase A's first cut can skip proxy support
  and add it as a follow-up if anyone hits the use case.
- New `Event::State(Resolving | Connecting)` plumbing.
- Tier 3: spin up a local mhxd container, hxnet TCP-connects,
  C-side fake-handler drives the rest of the legacy flow. Skips
  HOPE / TLS.

### Phase B — TLS handshake (already done on `claude/r3.3e-tls-hxnet`)

The four commits on `claude/r3.3e-tls-hxnet` slot in here. The
spawn-FD variant becomes the post-`Connecting` step in Phase A's
state machine.

### Phase C — Magic exchange in Rust
`~80 LOC src + ~100 LOC tests, half-day`

- After TCP/TLS connect, hxnet writes `HTLC_MAGIC` and reads back
  4 bytes. Mismatch → `Shutdown(MagicMismatch)`.
- Constants moved into the wire-protocol crate; C side already
  uses them.

### Phase D — LOGIN send (no HOPE, just chunks)
`~250 LOC src + ~200 LOC tests, 1 day`

- Use `hotline_proto::build` to assemble a LOGIN frame with the
  required chunks (login, password, optional client version /
  icon name).
- Password obfuscation is XOR-0xFF per byte — already in
  `hotline_proto`.
- Plaintext path only at first. HOPE chunks added in Phase F.

### Phase E — LOGIN reply receive
`~200 LOC src + ~200 LOC tests, 1 day`

- Parse the server's TASK reply via `hotline_proto::parse`.
- Extract HOPE chunks if present (SESSIONKEY, MAC_ALG list,
  CIPHER_ALG list, COMPRESS_ALG list, HOPE_APP_ID, HOPE_APP_STRING).
- Emit `Event::State(LoginReplyReceived)` to C with the parsed
  state. The C side gets enough info to validate the negotiated
  algorithms match its expectations.

### Phase F — HOPE handshake state machine + cipher transition
`~400 LOC src + ~300 LOC tests, 2-3 days`

- The crypto pieces all exist (`hxcrypto-hash` for HMAC,
  `hxcrypto-stream` for Blowfish-OFB-64, `hxcrypto-aead` for
  ChaCha20-Poly1305 + HKDF).
- New: the state machine that drives HOPE Step 1 (parse server
  challenge, compute response) and Step 2 (send response,
  receive server confirmation).
- The cipher transition is the tricky bit: at the end of HOPE,
  the connection's transport changes from "plaintext over
  TLS-or-TCP" to "Blowfish-OFB-64 over TLS-or-TCP" (or
  ChaCha20-Poly1305 / etc). The `transform::compose` helper
  already handles wrapping; we need a "rewrap mid-connection"
  operation. Likely a new `Transport` enum with method to swap
  out the cipher layer.

### Phase G — C-side rework
`~600 LOC delta in C, 2-3 days`

- `hx_connect` becomes a 30-LOC function that fills the config
  struct and calls `hxnet_connection_open`. Everything else gets
  deleted.
- `gtkhx_connect_ctx`, all its state-machine callbacks, gone.
- `send_login`, gone.
- `rcv_task_login`, gone.
- `tls_accept_certificate` becomes the `on_verify_cert` callback
  shipped on the TLS branch.
- `control_*` helpers and the GPollable read/write loop get
  deleted (this was R3.3.e-4f's goal — landing the Phase G
  finally unblocks it).

### Phase H — Documentation + rollout
`~50 LOC docs, half-day`

- Update `RUST-ROADMAP.md` Phase R3.x with the new lifecycle.
- Update `CLAUDE.md` to describe the new shape.
- Notes for the bookmarks-with-old-flags compat shim, if any
  bookmark format change shipped along the way.

**Total: ~1500 LOC Rust + ~600 LOC C delta + ~700 LOC tests across
seven branchable commits. Calendar time: 1-2 weeks of focused work
with code review.**

---

## 4. Open questions

- **SOCKS proxy support.** Today the C path picks SOCKS up free
  via GIO. Phase A's first cut skips it; if users hit "TLS works
  but SOCKS doesn't," add `tokio-socks` (~50 LOC integration)
  as a Phase A-1.
- **IPv4 vs IPv6 preference.** GIO defaults prefer IPv4. `tokio`
  defaults to whatever `lookup_host` returns first (typically
  v6-first on Linux). Need to either match the legacy preference
  or document the change. Probably keep the legacy IPv4-first
  behaviour to avoid surprising users on dual-stack networks.
- **Reconnect semantics.** The current C-side reconnect path
  reuses `htlc_conn` and re-runs `hx_connect`. The new
  `hxnet_connection_open` model should reuse the same shape —
  the FFI handle lifetime is bounded by one connection attempt.
- **Tracker connect.** Out of scope. Tracker uses a different
  wire protocol over a separate TCP connection (and optionally
  TLS, per the Tracker v3 TLS work that already shipped). It
  stays on its existing GIO path.
- **HTXF subchannel.** Stays on legacy GIOStream + `htxf_io_*`
  for now (already TLS-aware as of the May 2026 work). Moving
  HTXF into hxnet is a future phase; not blocked by this work.
- **Voice DTLS path.** Out of scope. Voice uses GStreamer's
  `webrtcbin` which has its own DTLS-SRTP stack.

---

## 5. Decisions to lock in

1. **Single entry point — `hxnet_connection_open(config)`.** The
   existing `spawn_fd_*` variants stay for tests that want to
   inject a pre-connected socket, but production goes through
   `_open`.
2. **Event-driven state reporting via `Event::State(...)`.** The
   C side doesn't poll for state; it gets transition events on
   the existing callback channel.
3. **No bookmark format change.** Existing bookmark on-disk
   layout is preserved; the existing tls flag (byte 4) drives
   the `config.tls`.
4. **Plaintext path first, then HOPE, then TLS-via-hxnet.**
   Phase order: A (TCP) → C (Magic) → D (Login send) → E (Login
   recv) → F (HOPE) → G (C rework + TLS integration via Phase B)
   → H (docs). Validating each piece against a server (mhxd for
   plaintext-no-HOPE, mhxd or hlserver.com for HOPE without TLS,
   Janus for TLS) gives an end-to-end test target at each step.
5. **Existing R3.3.e-tls-hxnet branch slots in as Phase B.** Don't
   throw away the TLS adapter work; integrate it into the
   lifecycle state machine when Phase F lands and we need to do
   the TLS handshake as part of the connect flow.

These pin the v1 behaviour. All five are reversible if anything
turns out wrong during implementation; explicitly out of scope
for re-debate during Phases A-H.
