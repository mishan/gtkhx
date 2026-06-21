# Phase G — `hx_connect` Migration Plan

> Sibling document to `docs/hxnet-connection-lifecycle-scoping.md`.
> The scoping doc says **what** Phase G does; this doc says
> **how** to do it without breaking the C side's wire-format
> assumptions.

## Status

- **Phase G part 1**: shipped on branch `claude/r3.3e-hxnet-open-ffi`.
  New FFI symbol `hxnet_connection_open_plaintext` wires the
  Phase G-prelude lifecycle orchestrator
  (`crate::lifecycle::run_plaintext_lifecycle`) onto the
  existing callback-mode handle plumbing
  (`wire_callback_state_with_on_state`).
- **Phase G part 2**: this doc.
- **Phase G part 3**: the actual C-side surgery, in its own
  branch with a live-server smoke test.

## The integration challenge

`hx_connect` (src/network.c L1897-2043) is ~146 LOC. That
number undersells the scope. The C code's connect flow has
state spread across:

| Where                     | What                                          |
|---------------------------|-----------------------------------------------|
| `struct gtkhx_connect_ctx` | per-attempt state (serverstr, login, pass, cancel, GSocketConnection, magic buf) |
| `htlc->fd`                | the connected socket — read by everything that calls `g_socket_*` directly |
| `htlc->in` / `htlc->out`  | qbuf-style read / write buffers driven by the rcv state machine |
| `htlc->rcv`               | function pointer into the rcv state machine (`hx_rcv_hdr` initially) |
| `current_conn`            | GSocketConnection refcounted into a module global; unrefd on close |
| `current_cancel`          | GCancellable for the in-flight connect |
| `connected`               | global flag the toolbar / banner code reads |
| `htlc->serverhost` / `htlc->serverport` / `htlc->tls` | stamped here so HTXF subchannel reuse works |
| `htlc->trans`             | reset to 1 here |
| `htlc->macalg`            | seeded with the strongest HOPE pref |
| `htlc->ip_addr`           | populated from the remote endpoint |
| `htlc->login`             | strcpy'd from ctx->login |

Of those, the orchestrator only owns the socket and the
LOGIN-frame writes. Everything else has to keep being set
the way the rest of the codebase expects — `rcv.c`,
`commands.c`, `xfers.c`, `banner.c`, `users.c`, the toolbar
all read from these fields directly.

Reads through `htlc->fd` are the gnarliest. After the
orchestrator takes the socket the C side cannot do
`g_socket_*` calls on it — the actor's tokio task owns the
read+write half. Today's hxnet-bridge install (R3.3.e-4)
already addresses this, but only after HOPE/login has
finished on the C side. Phase G inverts that: hxnet owns the
socket from byte zero, the C side never has it.

## The "what does the LOGIN reply tell us" problem

The orchestrator consumes the LOGIN reply itself
(`crate::login_reply::recv_login_reply`). Today's C code
consumes the reply through `rcv_task_login` and copies
fields out of it onto `htlc`:

- server `HTLS_DATA_VERSION` → `htlc->version`
- server `HTLS_DATA_BANNER_ID` → triggers banner fetch
- server `HTLS_DATA_SERVERNAME` → display string
- LOGIN reply task error → connect_fail + dialog

If the orchestrator swallows the reply silently, the C side
loses these. Three options for surfacing them:

### Option A — payload on `HandshakeDone`

Extend `Event::State(HandshakeDone)` to carry the parsed
`LoginReply`. The FFI's `on_state` callback gains a
`reply_chunks_ptr / reply_chunks_len` payload pair when
state == HXNET_STATE_HANDSHAKE_DONE.

**Pros**: explicit; the C side parses the chunks just like
`rcv_task_login` did, no flow change.

**Cons**: payload-bearing state events are a new shape; the
existing on_state callback signature is `(handle, state, ud)`
with no payload.

### Option B — replay the LOGIN reply as a Frame event

The orchestrator emits the LOGIN-reply bytes back to the C
side as a synthetic `Event::Frame` before emitting
`HandshakeDone`. The C side's existing `bridge_on_event_cb`
+ rcv.c dispatch table consume it through the normal LOGIN
reply dispatch (`rcv_task_login`).

**Pros**: zero changes to the FFI shape, zero changes to
rcv.c, the entire LOGIN reply dispatch path stays unchanged.
The orchestrator is purely additive transport.

**Cons**: the orchestrator has to remember the full reply
bytes (already does, via the `LoginReply` struct + the raw
frame in the parser path) and re-emit them. Minor
double-work — chunks get parsed once in Rust to decide
success / failure, then a second time in C for the side
effects.

### Option C — keep magic + LOGIN on the C side

The orchestrator stops at "TCP connected" (Phase A's
existing scope, already shipped). C side does magic + LOGIN
itself as it does today.

**Pros**: smallest delta; reuses everything.

**Cons**: defeats Phase G's goal. The whole point is
hxnet-owns-the-lifecycle so the C side's GIO/GPollable
machinery can be deleted. Option C means Phase G part 3 is
just "delete connect_ctx", which doesn't earn the new code.

**Recommendation: Option B**. It's the lowest-risk shape —
the C side's LOGIN reply dispatch is large and well-tested,
and a synthetic Frame event lets it run unchanged. The
orchestrator gains a small helper that holds onto the parsed
reply frame's wire bytes (it already reads them; just need
to not drop them) and ships them through `evt_tx` as
`Event::Frame` before `HandshakeDone`.

## Replacement order — three sub-branches

### `claude/r3.3e-phase-g-orchestrator-replay`

Implement Option B in the orchestrator:

- `recv_login_reply` already reads the full frame (header +
  body) before parsing chunks. Today it discards the wire
  bytes after parsing.
- Modify to retain the wire bytes in the returned
  `LoginReply` struct (new field
  `pub raw_frame: Vec<u8>`).
- In `run_plaintext_lifecycle`, after a successful login
  reply, emit `Event::Frame(Frame::from_raw(reply.raw_frame))`
  before `Event::State(HandshakeDone)`.

Tier 1 test: existing
`plaintext_lifecycle_happy_path` extends to assert that the
event stream includes `Event::Frame` carrying the LOGIN
reply's wire bytes between `LoginReplyWait` and
`HandshakeDone`.

Risk: low. Pure additive on the Rust side.

### `claude/r3.3e-phase-g-c-connect-via-orchestrator`

Add a new C function `hx_connect_via_orchestrator` that
sits next to `hx_connect` but uses the new FFI. Gate on a
new env var `GTKHX_NEW_CONNECT=1`:

```c
void
hx_connect (struct htlc_conn *htlc, const char *serverstr, guint16 port,
            const char *login, const char *pass, char secure, char tls)
{
    const char *env = g_getenv ("GTKHX_NEW_CONNECT");
    if (env && *env && !secure && !tls) {
        hx_connect_via_orchestrator (htlc, serverstr, port, login, pass);
        return;
    }
    /* ...existing path... */
}
```

`hx_connect_via_orchestrator` body (~80 LOC):

1. Common preamble: stamp `htlc->serverhost`, `htlc->serverport`,
   `htlc->tls = 0`, reset `chat_history_last_msgid`, clear
   `current_cancel`, hx_clear_chat, emit
   `GTKHX_CONNECTION_CONNECTING`.
2. Build `hxnet_connection_open_plaintext` callbacks. These
   are sibling functions to the existing bridge_on_*_cb
   triplet — same shape, NEW state callback that translates
   `HXNET_STATE_*` constants to `GTKHX_CONNECTION_*` signal
   values and emits them on the GtkhxSession.
3. On `HXNET_STATE_HANDSHAKE_DONE`: install the bridge
   handle into the global slot (same handle the existing
   path uses) so subsequent `hlwrite` calls route through
   `hxnet_connection_send_frame`. Set `htlc->trans = 1`,
   `htlc->fd = -1` (sentinel — nothing should read it),
   `connected = 1`.
4. Call `hxnet_connection_open_plaintext` with credentials.

Tier 1 test: `tests/integration/test_hxnet_open_smoke.c`
— set GTKHX_NEW_CONNECT=1, point at mhxd container, verify
LOGIN succeeds and the post-login Frame events flow.

Risk: medium. Plumbing is mostly mirror of the existing
post-handshake install path. The `htlc->fd = -1` sentinel
is the risky bit — if anything calls `g_socket_*` on it
we'll see EBADF crashes. Grep for `htlc->fd` reads in the
control-plane code path:

```
$ rg 'htlc->fd\b' src/ | wc -l
```

(Quick survey shows ~40 references; most are guarded by
`if (htlc->fd) { hx_htlc_close(...) }` close-time
patterns. The dangerous ones are the GIOChannel watch
installs in `gtkhx.c::hxd_fd_set` — these were
already wrapped after R3.3.e-4. Audit needed for any
straggler.)

### `claude/r3.3e-phase-g-delete-old-connect`

Once GTKHX_NEW_CONNECT=1 has been validated against:

- mhxd (1.x server with HOPE support)
- hlserver.com (1.0/1.2 fallback)
- Janus (1.9 + chat-history extension)

…the gate flips to default-on, the env var becomes opt-OUT
(`GTKHX_OLD_CONNECT=1` keeps the legacy path), and a
follow-up branch deletes:

- `struct gtkhx_connect_ctx` and its 11 fields
- `connect_ctx_free`
- `connect_fail`
- `send_login`
- `populate_htlc_remote_ip`
- `magic_timeout_cb`
- `on_async_connected`
- `on_magic_sent` / `on_magic_replied` / etc.
- `rcv_task_login` (replaced by the synthetic Frame
  replayed by the orchestrator + the existing post-login
  dispatch in rcv.c)
- the GPollable read/write source plumbing
  (`control_arm_read_source`, `control_write_drain`, etc.)
- `on_socket_client_event` (the TLS accept-cert handler —
  becomes the Rust `on_verify_cert` callback once TLS is
  folded into the orchestrator)

Estimated delete: ~600 LOC.

## What about TLS and HOPE?

Both block on orchestrator extension:

- **TLS in orchestrator**: `claude/r3.3e-tls-hxnet` already
  has the rustls handshake building blocks. The integration
  is: orchestrator gains a `tls_config: Option<HxnetTlsConfig>`
  parameter; when set, after `resolve_and_connect` returns
  `TcpStream`, wrap in `tokio_rustls::client::TlsStream`
  before magic exchange. State event ordering becomes
  Resolving → Connecting → Connected → **TlsHandshaking** →
  MagicExchange → ... .
- **HOPE in orchestrator**: extend `run_plaintext_lifecycle`
  to a `run_hope_lifecycle` variant that uses the Phase F
  step-1 builder, awaits the reply, runs Phase F step-2,
  derives keys via Phase F-2, wraps the transport in the
  chosen cipher adapter, then runs the actor.

Until these land, the env-var gate stays: TLS / HOPE
connections go through the legacy `hx_connect`, plaintext
connections opt into the new path.

## Test matrix before flipping the default

| Server          | Plaintext | HOPE | TLS  | Note                            |
|-----------------|-----------|------|------|---------------------------------|
| mhxd            | yes       | yes  | no   | local Tier 3 container          |
| hlserver.com    | yes       | no   | no   | 1.0/1.2 behaviour               |
| Janus           | yes       | yes  | yes  | 1.9 + chat-history extension    |
| Mobius          | no        | no   | yes  | separate-port TLS               |

Until the orchestrator supports HOPE + TLS, only the
**Plaintext** column drives the new path. Plaintext-against-
mhxd is the gate for `claude/r3.3e-phase-g-c-connect-via-orchestrator`.
Plaintext-against-hlserver.com is the gate for
`claude/r3.3e-phase-g-delete-old-connect` flipping the
default.

## Why this isn't shipping tonight

Three risk axes:

1. **No live-server smoke**: the C-side surgery cannot be
   meaningfully verified without driving mhxd / hlserver in
   a Tier 3 environment. Doing the surgery without the
   smoke is "I think it compiles, let me know in 12 hours
   if it crashes".
2. **`htlc->fd` audit risk**: the sentinel pattern works
   only if no code path dereferences the fd post-orchestrator-
   handoff. Audit needs to be careful.
3. **LOGIN reply replay correctness**: Option B above means
   the orchestrator's `LoginReply` parser and the C-side
   `rcv_task_login` dispatch see the same bytes. Any drift
   between them surfaces as missing `htlc->version` /
   missing banner / missing servername fields. Want a Tier
   3 test that asserts equivalence before flipping.

Each axis individually is manageable; together they're a
ship-Friday-skip-the-weekend size, not a ship-overnight size.

## Summary of branches the night left behind

| Branch                                       | What it ships                              |
|----------------------------------------------|--------------------------------------------|
| `claude/r3.3e-tls-hxnet`                     | Phase B: tokio-rustls handshake building blocks |
| `claude/r3.3e-hxnet-tcp-connect`             | Phase A: ConnectionState + TCP connect + `hxnet_connection_open_tcp` FFI |
| `claude/r3.3e-hxnet-magic`                   | Phase C: magic exchange module |
| `claude/r3.3e-hxnet-login-send`              | Phase D: LOGIN frame builder + send |
| `claude/r3.3e-hxnet-login-recv`              | Phase E: LOGIN reply receive + parse |
| `claude/r3.3e-hxnet-hope`                    | Phase F: HOPE handshake wire shape |
| `claude/r3.3e-hxnet-cipher-transition`       | Phase F-2: HOPE post-handshake key derivation |
| `claude/r3.3e-hxnet-lifecycle-plaintext`     | Phase G-prelude: end-to-end plaintext orchestrator |
| `claude/r3.3e-hxnet-open-ffi`                | Phase G part 1: `hxnet_connection_open_plaintext` FFI |
| **this doc** (Phase G part 2)                | migration plan for C-side rework |
| Phase G part 3+ (orchestrator-replay, C-connect, delete-old, …) | follow-up branches per matrix above |
