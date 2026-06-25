# Phase G — Rust connect-lifecycle migration (`hxnet` orchestrator)

> Part of the **Rust networking migration** (`docs/rust/ROADMAP.md`).
> "Phase G" is the step where the Rust `hxnet` crate takes ownership
> of the *entire* control-channel connect lifecycle — DNS, TCP,
> the magic handshake, LOGIN, and the LOGIN reply — instead of the
> legacy C `hx_connect` GSocketClient state machine.
>
> This doc began as a migration *plan*; the migration is now
> **complete**, so it reads as the design record plus a running
> status. The "Design rationale" and later sections preserve the
> decisions (Option A vs B, the trans/install ordering, the staged
> delete) for anyone touching the connect path later.

## Current status — complete

The orchestrator is the **only** control-channel connect path. There
is no legacy path and no gate left to flip.

| Step | State | Where |
|------|-------|-------|
| FFI foundation (`hxnet_connection_open_plaintext`) | shipped | `lifecycle::run_plaintext_lifecycle` + `wire_callback_state_with_on_state` |
| Option B reply replay | shipped | `LoginReply::raw_frame` → pre-`HandshakeDone` `Event::Frame` |
| C `hx_connect` on the orchestrator | shipped | `hx_bridge_install_orchestrated_plaintext` (trans pinning, `fd=-1` sentinel, synchronous install) |
| Capabilities negotiation | shipped | `HTLC_DATA_CAPABILITIES` (tag 0x01F0) advertised via the open FFI |
| HOPE + TLS in the orchestrator | shipped | `run_hope_lifecycle` + `run_plaintext_tls_lifecycle` (WebPKI→TOFU) |
| HOPE **no-cipher** (secure login over plaintext) | shipped | `select_algorithms` treats cipher as optional → `CipherLayer::None`; `claude/hope-no-cipher` |
| HTXF file-transfer subchannels on Rust | shipped | `hxnet::htxf` + `htxf_io.c` shim; `claude/htxf-h2-rewire` |
| Default flip → orchestrator-only | shipped | gate + `PHASE_G_DEFAULT_ON` removed in `delete-old-connect` |
| **delete-old-connect WAVE 1** (gate + legacy connect machinery) | shipped | `claude/delete-old-connect-exec` |
| **delete-old-connect WAVE 2** (install-over-socket + rcv HOPE branch + legacy `hlwrite` send path) | shipped | `claude/delete-old-connect-wave2` |
| **WAVE 3** — C crypto module removal (`hope.c`, `compress.c`, `cipher.c` audit) | **TODO** | see "C cipher / compress" below |
| Compression on the new path | not wired | orchestrator advertises empty compress list; follow-up |
| Post-login `rcv` coverage headless (increment 3) | blocked on R5 | see "Tier 3 coverage" below |

Validated end-to-end against live mhxd + Janus via
`tests/integration/test_real_connect.c` (`/real_connect/*`).

The remainder of this document is the **design record** behind those
decisions — kept because the connect path's silent-failure modes
(trans pinning, install ordering, the `fd=-1` sentinel) are subtle
and worth preserving for future work.

## The integration challenge

`hx_connect` (src/network.c, starting ~L2124) is ~146 LOC. That
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
   `if (htlc->fd) hx_htlc_close(...)` close-time guards firing.
   The sentinel is never passed to `close(2)` — `hx_htlc_close`
   tears down via `current_conn` / the hxnet handle, not
   `close(htlc->fd)`.)

4. **Build the callbacks.** Sibling functions to the existing
   `bridge_on_*_cb` triplet — the event callback routes
   through `hx_bridge_dispatch_frame` as today; the NEW state
   callback translates `HXNET_STATE_*` constants to
   `GTKHX_CONNECTION_*` signal values and emits them on the
   GtkhxSession. It does NOT itself set `connected = 1` — under
   Option B that's done by `rcv_task_login` when it processes the
   replayed LOGIN reply frame (which also handles version / banner /
   servername). The state callback only emits the coarse
   `GtkhxConnectionState` signals; on the failure states it routes to
   the shared failure handler (`connect_fail` today).

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

### Default-flip prep (historical — the gate has since been deleted)

> **Superseded by `delete-old-connect` (WAVE 1).** The gate, the
> `PHASE_G_DEFAULT_ON` constant, both env vars, and the three
> `/real_connect/gate_*` Tier 3 guards described below no longer exist —
> the orchestrator is the only path. Kept for the record of how the
> flip was staged.

The gate polarity is now centralized in
`src/network.c::hx_connect_use_orchestrator()`, driven by a single
constant `PHASE_G_DEFAULT_ON` in `src/network.h` (currently `0`).
Precedence: `GTKHX_OLD_CONNECT=1` (force legacy) beats
`GTKHX_NEW_CONNECT=1` (force orchestrator); with neither set,
`PHASE_G_DEFAULT_ON` decides. Both env vars are honored now so the
opt-out escape hatch is exercised *before* it becomes load-bearing.

Three deterministic Tier 3 guards pin the gate without needing a
server (`test_real_connect.c`): `/real_connect/gate_default`,
`/real_connect/gate_opt_in`, `/real_connect/gate_opt_out_wins`. They observe the
chosen path via the synchronous bridge-install signal (the orchestrator
installs the bridge inside `hx_connect`; the legacy path doesn't).
`gate_default` references the same `PHASE_G_DEFAULT_ON` the gate does,
so its expectation flips automatically with the constant.

**The actual flip** is then: change `PHASE_G_DEFAULT_ON` to `1` (one
line; `gate_default` updates with it). Do that only after the live
validation below is done — in particular a human clicking through a
real TLS trust-on-first-use dialog, which CI's auto-accept can't prove.
Asserting the downstream `rcv_task_login` effects (`htlc->version`,
banner fetch, SELFINFO timer) headlessly still needs the R5 rcv seam
(increment 3) — the headless test stubs `rcv_task_login`, so the
header-level replay assertion remains the strongest automated proof
until then.

### Connect-task timing parity with legacy

The orchestrator originally registered the "login" protocol task up
front in `hx_connect_via_orchestrator` (it had to exist before the
replayed reply could dispatch to it). That left the login task visible
in the Tasks window *concurrently* with the coarse "Connecting" task
for the whole connect — different from the legacy path, where the login
task appears only once the connection is up and credentials are going
out.

Root cause: the two paths emit `HANDSHAKE_DONE` at different moments.
Legacy emits it in `send_login` (magic done, login being sent) and then
registers the login task; the orchestrator emitted it at the very end
(after the login reply). So legacy means "entering login phase" while
the orchestrator meant "login complete."

Aligned by mapping the orchestrator's `LoginSending` state (already
emitted by `send_login` on the plaintext/TLS paths; now also emitted
after magic on the HOPE path) to the coarse `HANDSHAKE_DONE` view
transition, and registering the login task there via
`hx_orchestrator_register_login_task` (called from the bridge's
`LOGIN_SENDING` state callback). `LoginSending` is emitted strictly
before the replayed reply frame on the same ordered channel, so the
task is registered in time. Rust's end-of-handshake state no longer
drives a view transition; login completion is signalled by
`LOGIN_READY` (emitted by `rcv_task_login` on the replayed reply), as
in legacy. Net result: `CONNECTING` → `TCP_CONNECTED` → `HANDSHAKE_DONE`
(+ login task appears) → reply → `LOGIN_READY` — the same Tasks-window
sequence the legacy path produces.

### delete-old-connect — shipped in two waves

After the orchestrator was validated against mhxd (1.x + HOPE),
hlserver.com (1.0/1.2 fallback) and Janus (1.9 + chat-history), the
legacy path was removed. The escape hatch (`GTKHX_OLD_CONNECT`) and
the `PHASE_G_DEFAULT_ON` constant went with it — the orchestrator is
now unconditional. The deletion landed in two stacked branches.

**WAVE 1 — `claude/delete-old-connect-exec`** (~-2100 LOC). Removed:

- the gate itself: `hx_connect_use_orchestrator`, `env_flag_set`,
  the `GTKHX_OLD_CONNECT` / `GTKHX_NEW_CONNECT` env vars,
  `PHASE_G_DEFAULT_ON`;
- the legacy async-connect + magic state machine:
  `on_async_connected`, `on_magic_sent` / `on_magic_replied`,
  `send_login`, `connect_ctx_free`, **`connect_fail`**,
  `populate_htlc_remote_ip`, `magic_timeout_cb`;
- the legacy GIOStream read path: `control_arm_read_source`,
  `control_on_readable`, `htlc_stream_read`, `update_task`.

  *Correction to the earlier plan:* `connect_fail` was **not**
  retained. The orchestrator path routes failure through its own
  state-callback → `GtkhxSession` error signal, so the standalone
  legacy failure sink had no remaining caller and was deleted with
  the rest. `on_socket_client_event` / `tls_accept_certificate` *were*
  kept — the tracker's TLS connect (`tracker_fetch_connect` via
  `hx_sync_connect_to_host`) still uses them.

**WAVE 2 — `claude/delete-old-connect-wave2`** (~-450 LOC), two commits:

- *2a — install-over-socket path + rcv HOPE branch.* The old
  "install hxnet over an already-connected legacy socket" mechanism
  (`GTKHX_USE_HXNET` / `hx_install_hxnet_post_hope` / `hxnet_opt_in`
  + the deferred-install idle machinery + the bridge installers
  `hx_bridge_install_with_hope_state` / `_passthrough`) only existed
  to bridge the now-deleted legacy connect into hxnet, so it went.
  With it went the HOPE `if (pass)` branch of `rcv_task_login`
  (step-1 parse + key derivation + step-2 send) — unreachable now
  that the orchestrator registers the login task with `pass = NULL`
  and `run_hope_lifecycle` owns the crypto. Also the dead
  `gtkhx_connect_ctx` struct, `GtkhxConnectState` enum and
  `MAGIC_TIMEOUT_SEC`. (`struct tls_endpoint` stays — tracker uses it.)
- *2b — legacy `hlwrite` send path.* `hlwrite` / `hlwrite_chunks`
  now always ship through `hx_bridge_send_frame` (the bridge is
  always installed on a live session). The legacy else-branch and
  the whole GPollable write machinery
  (`control_arm_write_source`, `control_on_writable`,
  `control_remove_*_source`, `htlc_stream_write`, the source-id
  vars, `READ_BUFSIZE`, the unused `current_conn`) are gone.

**The rest of `rcv_task_login` stays.** Its `!pass` post-login body
(version / banner / servername / capabilities / `USER_CHANGE` /
SELFINFO-timer) is exactly what the replayed Frame runs under
Option B — deleting the whole function would strand every post-login
side effect.

### WAVE 3 — C cipher / compress module removal (TODO)

Both blockers that previously kept the C-side crypto modules alive
are now cleared. hxnet's Rust transform stack
(`BlowfishStream` / `AeadStream` / `GzipStream` / `Lz4Stream` /
`ZstdStream`) ciphers and compresses **all** control-channel traffic,
and the **HTXF file-transfer subchannels are now on Rust too**
(`hxnet::htxf` + the `htxf_io.c` shim; `cipher_aead.c` was already
removed when that landed). So the C crypto modules have lost, or are
about to lose, their last consumers:

- **`compress.c`** — no consumer outside the (now-Rust) control
  channel. Removable in full.
- **`hope.c`** — the C-side HOPE handshake helpers
  (`hope_parse_step1_reply` / `hope_build_login_field` /
  `hope_store_chain_keys` / `hope_build_alg_reply`). Their last
  caller was the `rcv_task_login` HOPE `if (pass)` branch, deleted
  in WAVE 2a. Verify no stragglers, then remove.
- **`cipher.c`** (Blowfish) — needs a consumer audit. HTXF moved to
  Rust, so confirm nothing on the C side still pulls in `cipher.h`
  before removing.

(RC4 is already gone — `claude/remove-rc4`. `bookmark_rc4_dialog.c`
is just a GTK prompt that detects the retired RC4 *bookmark byte* and
asks the user to pick a replacement; it includes `bookmark_cipher.h`,
not `cipher.c`, so it keeps no crypto alive.)

This is the natural next branch (`claude/delete-old-connect-wave3` or
similar). It is **gated on a careful dead-consumer audit** — remove
the entry points, then let `-Wunused-function` and the linker surface
the now-dead statics, deleting iteratively (the same compiler-guided
method WAVE 1 and 2 used). Extern functions don't warn, so they must
be removed explicitly.

## What about TLS and HOPE? (shipped)

Both have landed in the orchestrator — the env-var gate no
longer splits by transport; plaintext, HOPE and TLS all run
through the orchestrator on the new path.

- **TLS in orchestrator** (shipped): `resolve_and_connect`'s
  `TcpStream` is wrapped in `tokio_rustls::client::TlsStream`
  before the magic exchange, with a WebPKI-first verifier that
  falls back to the C-side TOFU known-hosts callback only when
  WebPKI validation fails. State ordering is
  Resolving → Connecting → Connected → **TlsHandshaking** →
  MagicExchange → … . See `tls.rs` / `run_plaintext_tls_lifecycle`.
- **HOPE in orchestrator** (shipped): `run_hope_lifecycle`
  uses the Phase F step-1 builder, awaits the reply, runs
  step 2, derives keys via Phase F-2, wraps the transport in
  the chosen cipher adapter (Blowfish-OFB-64 or
  ChaCha20-Poly1305), then runs the actor. HOPE-over-TLS is
  rejected up front (redundant double-encryption).

Compression is the one HOPE sub-feature still on the C side
only: the orchestrator advertises an empty compress_algs list
and composes with `CompressionKind::None`, so no connection
negotiates compression on the new path. Wiring it is a
self-contained follow-up (see `hope::select_algorithms`).

## Test matrix before flipping the default

| Server          | Plaintext | HOPE | TLS  | Note                            |
|-----------------|-----------|------|------|---------------------------------|
| mhxd            | yes       | yes  | no   | local Tier 3 container          |
| hlserver.com    | yes       | no   | no   | 1.0/1.2 behaviour               |
| Janus           | yes       | yes  | yes  | 1.9 + chat-history extension    |
| Mobius          | no        | no   | yes  | separate-port TLS               |

The orchestrator now supports all three columns. mhxd
(plaintext + HOPE, both ciphers) and Janus (plaintext + caps +
TLS) are green in Tier 3 on the new path. The remaining hole is
an **automated 1.0/1.2 regression**: there's no 1.0/1.2 server
in the container matrix (mhxd is 1.x-with-HOPE, Janus is 1.9),
so that behaviour is covered by manual smoke against old-Mac
servers (RetroMac / MacDomain) plus the pre-TASK-frame tolerance
in `login_reply.rs`. A 1.0/1.2 mock Tier 3 target is a
nice-to-have follow-up — valuable as a regression guard, less so
than a real server.

**The default has been flipped** (`PHASE_G_DEFAULT_ON = 1`).
`GTKHX_OLD_CONNECT=1` remains the escape hatch so the legacy path
is A/B-testable during the bake; `delete-old-connect` removes it
once the bake completes.

## Why this didn't ship in one night (historical)

> All three risk axes below were worked through and the migration
> shipped. Kept as the record of what made the C-side surgery
> delicate — the same hazards apply to anyone touching the connect
> path again.

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
   `real_connect_hxnet`, `real_connect`) — these *do* drive
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
headless against production code (that's what `real_connect`
does), but **post-login protocol handling** (chat/news/file
round-trips) can't, until the UI coupling in `rcv.c` is unwound.

### Plan — three increments, smallest payoff first

1. **Negotiation assertion in `real_connect`** — ✅ shipped
   (`5555249`). `/real_connect/capabilities_negotiated` drives the
   production orchestrator against a cap-aware matrix server (Janus)
   and asserts the server echoed `HTLC_DATA_CAPABILITIES` back —
   the direct guard for the class of bug above. The recording
   `hx_rcv_hdr` grew a two-phase handoff so the LOGIN reply body is
   inspectable; a `login.rs` unit test
   (`build_login_frame_advertises_capabilities`) is the send-side
   guard. ~70 LOC across the two.

2. **Route the harness's connect + login through the orchestrator.**
   *(Shipped.)* `hxnet`'s lifecycle is GTK-free, so the harness now
   drives connect / magic / LOGIN / negotiation through it instead of
   its hand-rolled raw-socket version. Two pieces landed:

   - **Foundation** — `hxnet_connection_open_plaintext_polling` (a
     polling-mode sibling of `open_plaintext` that keeps the event
     receiver for synchronous draining instead of the GLib callback
     forwarder), proven by `tests/integration/test_orchestrator_harness.c`.

   - **Bulk conversion** — an *orchestrated transport* in
     `integration_harness.c`, selected at runtime by
     `GTKHX_HARNESS_ORCHESTRATED`. The login entry points
     (`integration_open_login_or_skip` / `_to_caps_or_skip`) open via
     the polling FFI and return a synthetic fd in the `ORCH_FD_BASE`
     range; `integration_send` / `integration_recv_message` /
     `integration_close` detect that range and route through the actor
     (`send_frame` / `try_recv_frame` / `destroy`), rebuilding the
     22-byte header so every downstream chunk walker sees a
     byte-identical `htlc->in`. Real fds (xfer data channels, tracker
     sockets) stay below the base on the legacy path. No per-test
     changes; the full Tier 3 suite passes under both transports. CI
     runs the integration suite twice (legacy + orchestrated) so the
     production connect+login path is continuously exercised against
     the live servers. HOPE/TLS open helpers are not routed (they
     drive their own crypto/transport) and remain covered by
     `test_real_connect`.

3. **Post-login coverage on production `rcv`.** Blocked on R5 (UI →
   Rust) or a headless `rcv` seam that lets the dispatch handlers run
   without a live widget tree. Not worth forcing before R5; tracked
   here so the dependency is explicit.

Recommendation: keep increment 1 as the merge gate for capability
regressions, increment 2 is shipped (the suite runs under both
transports in CI), and let increment 3 fall out of R5.

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
