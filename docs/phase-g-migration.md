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
- **Phase G part 3**: shipped on branch `claude/r3.3e-phase-g`
  (plaintext, gated behind `GTKHX_NEW_CONNECT`). Option B replay in
  the orchestrator (`Frame::from_raw` + `LoginReply::raw_frame` +
  the pre-`HandshakeDone` `Event::Frame`); the C-side
  `hx_connect_via_orchestrator` + `hx_bridge_install_orchestrated_plaintext`
  per the corrected sketch below (trans pinning, `fd=-1` sentinel,
  synchronous bridge install, state-callback mapping). Capabilities
  (`HTLC_DATA_CAPABILITIES` = 0x001F) are advertised through the
  `open_plaintext` FFI so extensions negotiate; `htlc->ip_addr` is
  seeded from the server string. Validated end-to-end against live
  mhxd + Janus via `tests/integration/test_phase_g_connect.c`
  (`/phase_g/orchestrator_login` + `/phase_g/capabilities_negotiated`).
  Still gated on the full matrix (incl. hlserver.com + HOPE/TLS in
  the orchestrator) before the default flips — see "Test matrix
  before flipping the default" and "Tier 3 coverage of the
  production connect path" below.

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
The orchestrator is purely additive transport. *(Caveat,
see Recommendation: "zero changes to rcv.c" only holds if the
C side registers the login task under the orchestrator's
LOGIN trans and installs the bridge before the Frame is
replayed. Those aren't rcv.c changes, but they are required
new glue.)*

**Cons**: the orchestrator has to remember the full reply
bytes (already does, via the `LoginReply` struct + the raw
frame in the parser path) and re-emit them. The double-work
is smaller than it looks — Rust parses only far enough to
read the task-error bit (success vs failure); the rich fields
(version / banner / servername) are parsed once, in C. The
real cost is the trans-pinning + install-ordering glue, both
of which fail silently — see the sketch and Recommendation
below.

### Option C — keep magic + LOGIN on the C side

The orchestrator stops at "TCP connected" (Phase A's
existing scope, already shipped). C side does magic + LOGIN
itself as it does today.

**Pros**: smallest delta; reuses everything.

**Cons**: defeats Phase G's goal. The whole point is
hxnet-owns-the-lifecycle so the C side's GIO/GPollable
machinery can be deleted. Option C means Phase G part 3 is
just "delete connect_ctx", which doesn't earn the new code.

**Recommendation: Option B to bridge, Option A as the
destination.**

Option B is the right *migration* mechanism: it keeps the
receive side byte-identical to the legacy path, so the
coexistence ship can A/B the new transport against the old
with zero behavioural delta on everything downstream of the
LOGIN reply. The orchestrator gains a small helper that holds
onto the parsed reply frame's wire bytes (it already reads
them; just need to not drop them) and ships them through
`evt_tx` as `Event::Frame` before `HandshakeDone`.

But B is *not* zero-cost and is *not* the clean end state —
contrary to the "zero changes to rcv.c / zero FFI changes"
framing earlier in this doc:

- It needs the trans-pinning + synchronous-install glue from
  the `hx_connect_via_orchestrator` sketch above, both of
  which fail *silently* when wrong. That glue is throwaway —
  it exists only to make the legacy dispatch path swallow a
  frame it never sent.
- `rcv_task_login` cannot be deleted (see the corrected
  delete list below). Only its HOPE `if (pass)` branch and
  the send/connect orchestration around it go away; the
  `!pass` post-login body (version / banner / capabilities /
  `USER_CHANGE` / SELFINFO timer) is the whole reason the
  replayed Frame exists.

Option A (payload on `HandshakeDone`) is the cleaner
*destination*. It sidesteps both silent-failure axes — no
trans matching, no install ordering — because the reply never
rides the rcv dispatch path; the `HANDSHAKE_DONE` handler
gets the parsed chunks directly. Its one cost is factoring
the field-extraction out of `rcv_task_login` into a callable
parser, separate from the post-login orchestration. That
refactor is wanted anyway: once HOPE and TLS fold into the
orchestrator (`run_hope_lifecycle` + `tls_config`), the Rust
side already parses the reply to decide success, and the C
side wants a clean "parse-reply-fields" entry point decoupled
from "arm the post-login machinery". At that point A's
payload is just handing those already-parsed fields across,
instead of re-serializing them into a synthetic frame for C
to re-parse.

So: ship B for the plaintext coexistence/validation pass,
then converge on A as HOPE/TLS move into the orchestrator. If
the `rcv_task_login` field-extraction refactor is going to
happen regardless — and HOPE-in-orchestrator forces it —
going straight to A may be *less* net plumbing than carrying
B's trans-coordination glue through the HOPE work. Worth a
checkpoint after the plaintext-vs-mhxd gate: if B's glue
feels heavier than expected, that's the signal to skip to A
rather than carry B forward.

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

`hx_connect_via_orchestrator` body (~90 LOC). The ordering
below is load-bearing — steps 2, 3, and 6 exist only to make
the synthetic LOGIN-reply Frame (Option B) dispatch
correctly. Skip any one of them and login still "succeeds"
while every post-login side effect silently vanishes.

1. **Common preamble.** Stamp `htlc->serverhost`,
   `htlc->serverport`, `htlc->tls = 0`, reset
   `chat_history_last_msgid`, clear `current_cancel`,
   `hx_clear_chat`, emit `GTKHX_CONNECTION_CONNECTING`.

2. **Pin the LOGIN trans and register the reply task.** The
   LOGIN reply dispatches by transaction id —
   `hx_rcv_task` does `task_with_trans(trans)` (`rcv.c`
   ~L494) and a miss is a *silent* fallthrough (trans 0 =
   "no such task"). So the synthetic Frame only reaches
   `rcv_task_login` if a task is registered under the exact
   trans the orchestrator's LOGIN carries. The orchestrator
   owns the send, so the two sides must agree on the trans
   up front. LOGIN is always the first transaction — pin it
   to a constant shared by C and Rust (`HX_LOGIN_TRANS == 1`):

   ```c
   htlc->trans = HX_LOGIN_TRANS;          /* = 1 */
   task_new (htlc, RCV_TASK_FN (rcv_task_login), 0, 0,
             "login");                    /* stamps tsk->trans = 1 */
   htlc->trans = HX_LOGIN_TRANS + 1;      /* = 2 */
   ```

   The final bump matters: the post-login follow-up sends
   (`who` / `USER_GETLIST` / chat-history) fire from inside
   `rcv_task_login` during the replayed-frame dispatch and
   stamp `tsk->trans = htlc->trans` (`tasks.c` L1114). Left
   at 1 they collide with the login task. The legacy path
   got `trans = 2` for free because the C-side LOGIN send did
   `htlc->trans++`; the orchestrator's send never touches the
   C counter, so we advance it by hand.

3. **Set the fd sentinel to -1, not 0.**
   `hx_bridge_dispatch_frame` early-returns on
   `htlc->fd == 0` (`hxnet_bridge.c` L114) — that's the
   bridge's "connection closed, drop the frame" signal. The
   orchestrator owns the socket, so the C side has no real
   fd; use `-1` to mean "live but no C-visible fd". `0` would
   silently drop every replayed frame. (`-1` also keeps the
   `if (htlc->fd) hx_htlc_close(...)` close-time guards
   firing, and `close(-1)` is a harmless EBADF.)

4. **Build the callbacks.** Sibling functions to the existing
   `bridge_on_*_cb` triplet — the event callback routes
   through `hx_bridge_dispatch_frame` as today; the NEW state
   callback translates `HXNET_STATE_*` constants to
   `GTKHX_CONNECTION_*` signal values and emits them on the
   GtkhxSession. On `HANDSHAKE_DONE` it sets `connected = 1`
   (version / banner / servername are already handled by the
   replayed Frame's `rcv_task_login` run, which fires *before*
   this state event); on the failure states it calls the
   shared failure handler (`connect_fail` today).

5. **Call `hxnet_connection_open_plaintext`** with the
   credentials and the pinned `HX_LOGIN_TRANS`, capturing the
   returned handle.

6. **Install the handle into the global slot — synchronously,
   before returning.** This is what makes the
   Frame-before-`HandshakeDone` order (the orchestrator-replay
   branch) safe: `hx_bridge_dispatch_frame` also gates on
   `hx_bridge_is_installed()` (`hxnet_bridge.c` L114), so the
   bridge must be installed before the first event callback
   fires. We're on the GLib main loop here and callbacks
   arrive via the event ferry (`g_idle`), so they cannot run
   until `hx_connect_via_orchestrator` returns — installing
   synchronously after the open call closes the window. (This
   replaces the earlier draft's "install on `HANDSHAKE_DONE`",
   which would have dropped the LOGIN reply.)

Tier 3 test: `tests/integration/test_hxnet_open_smoke.c`
— set `GTKHX_NEW_CONNECT=1`, point at the mhxd container,
verify LOGIN succeeds *and* that `rcv_task_login` actually
ran: assert `htlc->version` is populated, the banner fetch
was triggered, and the SELFINFO fallback timer armed. A test
that only checks "login succeeded" passes even when the
replayed Frame was dropped — the silent-failure modes from
steps 2/3/6 hide there.

Risk: medium, and the dangerous part is *silent*. The
plumbing mostly mirrors the existing post-handshake install
path; the new failure axes are the trans pinning (step 2)
and install ordering (step 6), both of which let login
"succeed" while version / banner / servername / USER_CHANGE
never happen — hence the effect-level Tier 3 assertions
above are the real gate. The `htlc->fd = -1` sentinel is the
one loud-failure axis — if a straggler calls `g_socket_*` on
it we get EBADF. Grep the control-plane reads:

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
- `send_login`
- `populate_htlc_remote_ip`
- `magic_timeout_cb`
- `on_async_connected`
- `on_magic_sent` / `on_magic_replied` / etc.
- the HOPE `if (pass)` branch of `rcv_task_login` (step-1
  parse + key derivation + step-2 send) — dead once
  `run_hope_lifecycle` owns the crypto. **The rest of
  `rcv_task_login` stays.** Its `!pass` post-login body
  (version / banner / servername / capabilities /
  `USER_CHANGE` / SELFINFO-timer) is exactly what the
  replayed Frame runs under Option B, and what Option A
  would call directly. Deleting the whole function would
  strand every post-login side effect — the synthetic Frame
  has nothing else to dispatch to ("the existing post-login
  dispatch in rcv.c" *is* `rcv_task_login`).
- the GPollable read/write source plumbing
  (`control_arm_read_source`, `control_write_drain`, etc.)
- `on_socket_client_event` (the TLS accept-cert handler —
  becomes the Rust `on_verify_cert` callback once TLS is
  folded into the orchestrator)

**Retained, not deleted:** `connect_fail`. It's the shared
failure/dialog routine, and the orchestrator path's state
callback reuses it for the `HXNET_STATE_*` error states (step
4 of the sketch). When the legacy connect flow goes away,
`connect_fail` stays as the single failure sink — or gets
inlined into the state callback, but it doesn't disappear.

Estimated delete: ~550 LOC (most of `rcv_task_login`'s bulk
is its HOPE branch; the post-login body that survives is
small).

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
3. **LOGIN reply replay correctness**: under Option B the
   rich fields (`htlc->version`, banner id, servername) are
   parsed exactly *once*, on the C side, by `rcv_task_login`
   — the orchestrator only parses far enough to read the
   task-error bit and choose `HandshakeDone` vs `Failed`. So
   the duplicated surface is just the success/failure
   verdict, not the field set; the only equivalence to pin is
   "Rust's success verdict == C's `task_inerror`". The
   likelier silent-failure modes aren't parser drift at all —
   they're the trans mismatch (step 2) and install ordering
   (step 6) from the sketch above, i.e. the reply never
   reaching `rcv_task_login`. So the Tier 3 gate asserts the
   *effects* (`htlc->version` populated, banner fetch
   triggered, SELFINFO timer armed), which catches both
   classes at once.

Each axis individually is manageable; together they're a
ship-Friday-skip-the-weekend size, not a ship-overnight size.

## Tier 3 coverage of the production connect path

A capabilities-negotiation regression shipped on the orchestrator
path (the LOGIN omitted `HTLC_DATA_CAPABILITIES`, so chat-history /
inline-media / voice never negotiated) and **no Tier 3 test caught
it**, even though the suite has chat-history coverage. Worth writing
down why, and the plan to close the gap.

### Why the existing Tier 3 suite doesn't exercise production connect

There are effectively **two client wire implementations** in the
tree:

1. **`tests/integration/integration_harness.c`** — used by
   `test_login`, `test_chat_history`, the HOPE tests, and ~30 others.
   It opens a raw socket and hand-rolls its *own* magic + LOGIN +
   recv loop (`integration_login_guest_caps` builds its own chunk
   list). It links `proto_helpers` + `cipher` but **not** `network.c`
   / `rcv.c`. So "chat-history works" proves the *server* supports
   the extension against the *harness's* login — it never touches the
   production client's LOGIN builder.
2. **`connect_test_stubs.c`** tests (`real_connect`,
   `real_connect_hxnet`, `phase_g_connect`) — these *do* drive
   production `network.c::hx_connect`, but stub `rcv.c` + the UI.

The capabilities bitmask lives in the production LOGIN builders
(`login_packet.c` for legacy/HOPE — correct; `login.rs` for the
orchestrator — regressed). No Tier 3 test drove either production
builder *and* inspected the negotiated result, so the orchestrator
regression was invisible. The legacy path happened to be right.

### The blocker for "all of Tier 3 on production connect"

`rcv.c`. Every `HTLS_HDR_*` handler (`rcv_task_login` included) calls
directly into the GTK/libadwaita UI (chat, user list, news, files).
A headless test binary can't link that — which is *why* the harness
reimplements the wire path and why `connect_test_stubs.c` stubs
`rcv_task_login`. So **connect + login + negotiation** can run
headless against production code (that's what `phase_g_connect`
does), but **post-login protocol handling** (chat/news/file
round-trips) can't, until the UI coupling in `rcv.c` is unwound.

### Plan — three increments, smallest payoff first

1. **Negotiation assertion in `phase_g_connect`** — ✅ shipped
   (`5555249`). `/phase_g/capabilities_negotiated` drives the
   production orchestrator against a cap-aware matrix server (Janus)
   and asserts the server echoed `HTLC_DATA_CAPABILITIES` back —
   the direct guard for the class of bug above. The recording
   `hx_rcv_hdr` grew a two-phase handoff so the LOGIN reply body is
   inspectable; a `login.rs` unit test
   (`build_login_frame_advertises_capabilities`) is the send-side
   guard. ~70 LOC across the two.

2. **Route the harness's connect + login through the orchestrator.**
   `hxnet`'s `open_plaintext` is GTK-free, so the harness could call
   it for connect / login / negotiation instead of its hand-rolled
   version, then read frames via the bridge. That makes the *login
   phase* of all ~30 existing Tier 3 tests exercise production
   networking for free, collapsing the two client implementations
   into one for the part that's testable headless. Bigger change —
   the harness is blocking-socket shaped and the orchestrator is
   async, so the harness's `integration_recv_message` family needs
   an actor/bridge-backed variant. Scoped as its own branch.

3. **Post-login coverage on production `rcv`.** Blocked on R5 (UI →
   Rust) or a headless `rcv` seam that lets the dispatch handlers run
   without a live widget tree. Not worth forcing before R5; tracked
   here so the dependency is explicit.

Recommendation: keep increment 1 as the merge gate for capability
regressions, do increment 2 on its own branch when the harness
async-recv work is worth it, and let increment 3 fall out of R5.

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
