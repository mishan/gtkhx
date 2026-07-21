# Untangling the remaining C network layer into Rust — scoping

> Status: **scoping / not started.** This is a survey + sequenced plan, not a
> committed design. It builds on Phase G (`phase-g-migration.md`), the
> `hotline-proto` crate (R2), and the `hxnet` orchestrator (R3). Nothing here
> re-litigates those; it picks up where they left off.

## Why this doc exists

Phase G moved the *transport* into Rust: `hxnet` owns connect → magic → LOGIN →
HOPE → TLS → framing → crypto → compression, and `hotline-proto` owns the wire
encoders/parsers. But the **protocol dispatch** — the part that decides "a frame
of type X with transaction T just arrived, match it to a pending request and do
something" — is still a C spine spread across `rcv.c`, `network.c`, and
`hxnet_bridge.c`, tied together by the `struct task` transaction table.

That spine is load-bearing for a large fraction of the app: `task_new` alone is
called from 14 files, and every receive handler funnels through `rcv.c`. Every
attempt to port a "leaf" that touches it — the Tasks model, per-domain receive
handlers, individual senders — runs into the same tangle: you can't cleanly move
one thread without the whole knot shifting. The recommendation is to stop
porting leaves that snag on it and instead untangle the spine deliberately,
in a sequenced way, so the downstream ports become clean afterward.

## What is and isn't "the network layer"

**In scope** (the spine this doc is about):

| File | Lines | Role |
|------|-------|------|
| `src/rcv.c` | ~2774 | The receive dispatch — a `htlc->rcv` function-pointer state machine, the `trans`→task matcher, and 39 `hx_rcv_*` / `rcv_task_*` handlers. |
| `src/network.c` | ~1481 | Connection lifecycle (`hx_connect` / `hx_htlc_close`), the send framing (`hlwrite` / `hlwrite_chunks` / `hlpack`), ping keepalive, the post-login state machine, TLS-TOFU verify trampolines, and the `htxf_connect` / `hx_tracker_list_async` glue into hxnet. |
| `src/hxnet_bridge.c` | ~968 | The C↔Rust boundary: takes `Event::Frame` from the Rust actor and stages it into `htlc->in` + calls `hx_rcv_hdr`; translates lifecycle state callbacks into `GtkhxSession` connection signals. Already the clean seam. |
| `struct task` / `sess->tasks` | `protocol.h` + `tasks.c`/`tasks_table.c` | The transaction table (`GHashTable<u32 trans, struct task*>`) shared by the send path (registers a reply callback) and the receive path (matches + invokes it). |

**Explicitly out of scope:**

- **`src/commands.c` (~686 lines) is NOT the send path.** It is the chat
  `/`-command dispatch (`/nick`, `/me`, `/msg`, `/server`, `/exec`, …). It
  belongs to the chat UI surface, not the network layer, and happens to call
  senders the way any UI action does. It should be ported alongside the chat
  window work, not here.
- The `gtask` Tasks-*window* GUI (rows / progress / buttons in `tasks.c`) — that
  is a view port, tracked separately.
- Anything already in Rust: `hxnet` (transport/crypto/compress/htxf/tracker),
  `hotline-proto` (wire codecs), `hxcrypto-*`, `hxcompress`.

## Current data flow (post Phase G)

**Receive:**

```
Rust hxnet actor  --Event::Frame(bytes)-->  hxnet_bridge.c
   bridge stages bytes into htlc->in, calls  rcv.c::hx_rcv_hdr()
   hx_rcv_hdr parses the 22-byte header, sets htlc->rcv = <body handler>
   body handler runs:
       - parses the body (increasingly via hotline-proto FFI extractors)
       - if HTLS_HDR_TASK: hx_rcv_task() looks up sess->tasks[trans],
         invokes task->rcv(htlc, ptr, data), removes the task
       - emits a GtkhxSession signal (or calls a view fn directly)
   resets htlc->rcv = hx_rcv_hdr for the next frame
```

**Send:**

```
some UI action / rcv handler builds chunks
   (increasingly via a hotline-proto Rust builder)
   task_new(htlc, rcv_cb, ...) registers the reply handler in sess->tasks
     keyed on htlc->trans (then trans is bumped)
   hlwrite()/hlwrite_chunks() in network.c packs the frame (hlpack)
   routes the bytes through hx_bridge_send_frame() -> hxnet actor
```

So `hxnet` is the transport, `hotline-proto` is the codec, and the **C spine is
the dispatcher + the correlation table** between the two.

## The knots (why it doesn't come apart one-thread-at-a-time)

**Knot A — the `struct task` transaction table is shared by send AND receive.**
The send path writes it (`task_new`), the receive path reads+drains it
(`hx_rcv_task`), teardown clears it (`hx_htlc_close` → `g_hash_table_remove_all`
→ `task_free` → optional `ptr_free`). It stores a `rcv_task_fn` **C function
pointer** per entry. You can't move the receive dispatch to Rust without the
table (Rust needs to find the callback), and you can't move the table without a
plan for that C callback. This table is the true center of the knot.

**Knot B — the `htlc->rcv` function-pointer state machine.** `hx_rcv_hdr` is a
mini state machine: it parses a header and points `htlc->rcv` at a body handler,
which resets it afterward. It's driven by staged bytes in `htlc->in`. Moving the
router to Rust means Rust owns the header parse + the "which handler" decision.

**Knot C — handlers mix three concerns.** Each `hx_rcv_*` handler does (1) wire
parsing, (2) task correlation, and (3) UI emission — often interleaved. The wire
parsing is already migrating to `hotline-proto`; the UI emission already has a
clean channel (`GtkhxSession` signals, per the CLAUDE.md signal taxonomy). But
the three are braided in the C, so a handler can't move wholesale until they're
separated.

**Knot D — `hlwrite`/`hlpack` framing + ~17 sender sites.** The outbound framing
lives in `network.c`; the per-request senders that call it are spread across
`chat.c`, `msg.c`, `users.c`, `files.c`, `xfers.c`, `banner.c`, `gif_icons.c`,
`usermod.c`, `options.c`, `inline_media_*`, `chat_history.c`,
`files_remote_provider.c`, and `rcv.c` itself. The framing is small; the spread
is the cost.

## Target end-state

- **`hxnet` (or a thin new `hxrecv` crate) owns the receive dispatch**: header
  parse + the `trans`→handler routing. `hotline-proto` already owns the body
  parse.
- **The transaction table is a Rust structure** with a C-ABI façade
  (`task_new` / `task_with_trans` / `task_delete` preserved) so the ~14 caller
  files are unchanged. The `rcv_task_fn` stays a C callback the Rust table stores
  and invokes — no behavioural change, wire-compat untouched.
- **Handlers become thin**: each `hx_rcv_*` handler's wire-parse is delegated to
  `hotline-proto`, its correlation is done by the Rust dispatcher, and its only
  remaining C is the `GtkhxSession` signal emission — which can then move to the
  relevant model/recv crate (as the news receive path already did with
  `hxnews-recv`).
- **`hlwrite`/`hlpack` framing moves into Rust**, so senders build+register
  +send through one Rust seam.
- **`hxnet_bridge.c` shrinks** to lifecycle/state plumbing once the dispatch no
  longer needs C-side `htlc->in` staging.

## Proposed sequence (each step a shippable branch, wire-compat preserved)

The ordering is chosen so each step reduces the tangle for the next, and so the
`struct task` spine (Knot A) is addressed early because everything depends on it.

**N0 — Handler inventory + dependency survey (doc only).** Enumerate all 39
`rcv.c` handlers with, per handler: opcode, whether the body-parse is already in
`hotline-proto` or still C, which `GtkhxSession` signal(s) it emits (or which
view fn it calls directly), and which per-session state it touches. This is the
map that tells us which handlers are already "thin enough" to move and which
need a wire-parse extraction first. Cheap, high-leverage, unblocks parallel work.

**N1 — Transaction table → Rust behind the existing C ABI (Knot A).** This is
the "tasks model to a crate" work, but scoped as *network spine* rather than a
standalone leaf. A `hxtask` crate owns the `GHashTable` (kept a real GHashTable —
`network.c`'s teardown iterates it directly) + `task_free`, exporting
`tasks_table_new` / `tasks_init` / `task_new` / `task_with_trans` / `task_delete`
unchanged. `struct task` layout stays C-owned (`protocol.h`) with a `#[repr(C)]`
mirror + `_Static_assert`. The crate externs the few C symbols it needs
(`sess_from_htlc`, the `sess->tasks` field via a tiny accessor shim, the
`task-update` signal emit, `gtask_delete_tsk`). Porting `test_task_hash.c` to
crate `#[test]`s comes for free. **This is the prerequisite that unblocks both
the send-framing move and the receive-dispatch move**, which is why it leads.

**N2 — Send framing into Rust (Knot D). _Mostly already done; the residual
step landed._** On investigation the send *serialization* was already Rust: the
byte-level wire encoder is `hotline-proto`'s `build::pack_message` /
`pack_message_size` (exported as `gtkhx_proto_pack_message*`), and the
chunk-array entry point `hlpack_chunks` (proto_helpers.c) was already a thin
wrapper around it. The one hold-out was the **variadic `hlpack`**, which still
hand-rolled the header + per-chunk `htonl`/`memcpy` loop in C. N2 unified it:
`hlpack` now marshals its varargs into an `hx_chunk` array and delegates to
`hlpack_chunks`, so *both* send entry points serialize through the single Rust
packer. The `proto - gtkhx:hlwrite` byte-level test confirms the output is
identical. What remains in C after N2 is not serialization but **htlc-lifecycle
glue** — the `htlc->out` qbuf growth, the `htlc->trans` bump, the send-side
`proto_trace` re-walk, and the `hx_bridge_send_frame` hand-off to the actor.
Those are tied to the C-owned connection state + the bridge, so moving them is
properly part of **N3** (which takes ownership of the connection I/O), not a
standalone send-framing step. N2 is therefore complete as the `hlpack`
unification; the rest folds into N3.

**N3 — Receive dispatch into Rust (Knot B). _Landed (two steps)._**

_Step 1 — routing._ The opcode→handler-kind decision — the core of the old
`hx_rcv_hdr` `switch`, including the composite-`TASK` mask some servers use —
moved into `hotline-proto::dispatch::route` (exported as `hx_recv_route`),
exhaustively unit-tested. Doing this **surfaced a latent bug**: `hotline-proto`'s
`ServerHdr` had `Chat`/`Msg` swapped (`Chat = 0x68` but hotline.h's
`HTLS_HDR_MSG = 0x68`); the values were unused so nothing routed on them, but
building the router on top of them would have inverted chat/msg with no test to
catch it — a concrete vindication of doing the testable routing core first.
Fixed, with the router's behavioural tests as the guard.

_Step 2 — retire the round-trip + the state machine._ The hxnet actor already
parses the header, yet the bridge re-packed it into `htlc->in` so `hx_rcv_hdr`
could re-decode it and run a two-phase `htlc->rcv` state machine (header handler
→ body handler). That's consolidated: the bridge now stages the whole frame
(header + body) into `htlc->in.buf` in one shot and calls a single
`hx_dispatch_frame(htlc, type, trans, flag, body_len)` in rcv.c, which traces,
routes via `hx_recv_route`, and calls the body handler directly. No re-decode,
no `htlc->rcv` state machine; `hx_rcv_hdr` is deleted. The handler-facing
`htlc->in` layout (22-byte header + body, `pos` past the body, `len == 0`) is
preserved byte-for-byte, so the handlers are unchanged.

_Still open in N3:_ moving `hx_rcv_task`'s `trans`→task match into Rust (the
table is already the `hxtask` crate, so the lookup can move; the `rcv` callback
stays a C fn pointer the Rust side invokes), and eventually dropping the
`htlc->in` staging entirely so handlers take `(body_ptr, body_len)` — but that
last step waits on the per-domain N4 extractions, which unbraid the handlers'
`htlc->in` reads one family at a time.

**Note on coverage:** the receive-dispatch→`GtkhxSession`-signal path has no
headless integration coverage (the Tier-3 tests do wire round-trips via
`integration_recv_message`; `real_connect` stubs the dispatch entry to record
the replayed frame — it validates the bridge→dispatch handoff + the LOGIN-reply
capability echo, but not the handlers' signal emission). So per-handler *signal*
coverage comes from extracting each N4 domain into a crate (the `hxnews-recv`
pattern), where the handler's parse→signal logic is unit-testable in isolation.
The N3 routing core is covered by its own `dispatch.rs` tests.

**N4 — Per-domain handler migration (Knot C), one domain per branch.** With the
skeleton in Rust and bodies parsed by `hotline-proto`, move each handler family
(chat, users, msg, files/xfers, banner/icons, voice, chat-history) so the
handler emits its `GtkhxSession` signal from the relevant model/recv crate —
exactly the shape `hxnews-recv` already established for the news receive path.
These are independent and parallelizable once N3 lands.

**N5 — Shrink the bridge. _First brick landed._** Once dispatch + framing are
Rust, `hxnet_bridge.c` collapses to connection-lifecycle/state plumbing; the
frame staging + header pack/dispatch code is deleted. First increment shipped:
the bridge's hand-rolled 22-byte header encoder (`hx_bridge_pack_header`) moved
into `hotline-proto` as `pack_header` / `gtkhx_proto_pack_header` (byte-for-byte,
reused by `pack_message`, unit-tested), and the C encoder was deleted — the
bridge now calls the Rust packer to reconstruct the header the actor already
parsed. The rest of "shrink/delete the bridge" is part of the larger endgame.

> **The full endgame — retiring `rcv.c`, the bridge, and C-owned `htlc_conn`
> — is scoped in [`network-endgame.md`](network-endgame.md).** N5's remaining
> work is subsumed by that plan's Phase E3 (delete the bridge), which depends on
> E1 (`htlc_conn` to Rust) and E2 (handlers to Rust, `rcv.c` deleted) first.

## Constraints & risks (carry-overs, don't re-decide)

- **Wire compat with 1.2 / 1.5 / 1.9 is a hard requirement.** Only the
  implementation moves; the negotiated bytes, the `trans` correlation semantics,
  and the header framing are frozen. Every step gates on Tier-3 `real_connect` +
  the proto suites staying green (against mhxd, Janus, the Mobius family).
- **The `rcv_task_fn` C-callback ABI is preserved** through N1–N3 — the Rust
  table/dispatcher stores and invokes C function pointers. We are moving the
  *plumbing*, not rewriting 39 handlers in one go.
- **`htlc->trans` stamping** is the correlation key; N1/N2 must keep the exact
  stamp-then-bump order (LOGIN is still pinned to `HX_LOGIN_TRANS`, per Phase G).
- **Single-connection scope** holds throughout (`sess_from_htlc` returns the one
  session); don't propagate the `MAX_CONN` abstraction while untangling.
- **`hx_rcv_task` error suppression** (login/reconnect task errors are toasted,
  not dialogged) is behaviour to preserve, not clean up, during the move.

## Recommended first increment

Do **N0** (the handler inventory) and **N1** (the transaction table crate)
together on one branch: N0 is a doc that makes N4 plannable, and N1 is the
concrete spine move that unblocks N2 and N3. That gives a shippable, testable
first step (crate + `test_task_hash` coverage + Tier-3 green) without yet
touching the 39 handlers, and it converts the deferred "tasks model to a crate"
idea into the load-bearing first domino of the network untangling rather than a
standalone leaf.
