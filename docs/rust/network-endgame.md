# Network endgame — retiring rcv.c, the bridge, and C-owned `htlc_conn`

> Successor to `network-untangling-scope.md`. That doc's N0–N5 untangled the
> receive **dispatch** and moved the wire codecs, the transaction table, and the
> per-handler signal emits into Rust. This doc is the endgame: the three
> remaining structural goals and the dependency-ordered path to them.

## The three end goals

1. **No more `hxnet_bridge.c`.** The C↔Rust receive seam disappears; the hxnet
   actor delivers frames straight into Rust receive handlers, and the bridge's
   lifecycle/connect-install duties fold into Rust.
2. **No more `rcv.c`.** Every receive handler — parse, task correlation, state
   mutation, and signal emit — lives in Rust. The `hx_dispatch_frame` opcode
   switch becomes a Rust match on the (already-Rust) route table.
3. **`struct htlc_conn` moves to Rust.** The connection struct is owned by Rust;
   handlers read and write it in Rust, and the C UI reaches whatever it still
   needs through a narrow accessor facade.

These are not independent — they're one chain. You cannot delete the bridge's
frame staging while C handlers still read `htlc->in`; you cannot move the
handlers to Rust while they mutate a C-owned `htlc_conn`. So the order is forced:
**`htlc_conn` first, handlers second, bridge last.**

## What already exists (the assets we build on)

The untangling has already put every *collaborator* the handlers touch behind a
Rust seam, which is what makes the endgame tractable rather than a rewrite:

| Concern | Where it lives now |
|---|---|
| Wire parse / build | `hotline-proto` (every `gtkhx_proto_parse_*` / `pack_*`) |
| Transaction table | `hxtask` crate (`task_new` / `task_with_trans` / `task_delete`) |
| Opcode → handler routing | `hotline-proto::dispatch::route` (`hx_recv_route`) |
| Per-handler signal emit | `hxchat-recv`, `hxuser-recv`, `hxmsg-recv`, `hxxfer-recv`, `hxagreement-recv`, `hxicon-recv`, `hxnews-recv` |
| View boundary (signals) | `GtkhxSession` (Rust `glib::subclass`) |
| Per-session collections | `HxChatRegistry`, `HxMemberModel`, `MediaTable` (Rust) |
| Transport / crypto / framing | `hxnet` + `hxcrypto-*` / `hxcompress` |
| Header wire-encode | `hotline-proto::build::pack_header` (`gtkhx_proto_pack_header`) |

So a receive handler's *only* remaining C is: (a) reading the parsed body out of
`htlc->in` by calling a `hotline-proto` FFI, (b) reading/writing scalar
`htlc_conn` fields, and (c) the task-correlation glue in `hx_rcv_task`. (a) and
(c) are already thin; (b) is the thing `htlc_conn`-to-Rust removes.

## Current receive data flow (post N1–N5)

```
hxnet actor  --Event::Frame(bytes)-->  hxnet_bridge.c::bridge_on_event_cb
  → hx_bridge_dispatch_frame: stage header+body into htlc->in
      (gtkhx_proto_pack_header re-packs the header the actor already parsed)
  → rcv.c::hx_dispatch_frame(type, trans, flag, body_len)
      → hx_recv_route(type)  [Rust]  → C handler fn pointer
      → handler(htlc): parse htlc->in [Rust FFI] → mutate htlc → emit [recv crate]
      → (TASK) hx_rcv_task: header_trans [Rust] → task_with_trans [Rust]
               → tsk->rcv(htlc, ...) [C callback] → task_delete [Rust]
```

The redundancy the endgame removes: the actor parses the header, the bridge
re-packs it into `htlc->in`, and C re-decodes it — three passes over the same 22
bytes — purely because the handlers read `htlc->in`. Once handlers take
`(&[u8] body, header fields)` in Rust, the staging + re-pack + re-decode all go.

## `struct htlc_conn` today — the weight

Defined in `protocol.h` (~150 lines). **55 distinct fields** referenced across
**32 C files**; the heavy readers are `network.c` (~125 refs) and `rcv.c` (~120),
then `proto_helpers.c`, `hxnet_bridge.c`, `inline_media*.c`, `gif_icons.c`,
`banner.c`, `files.c`. The field groups:

- **Live session identity/state**: `fd`, `uid`, `trans`, `chattrans`, `icon`,
  `color`, `nick_color`, `version`, `access`, `name`, `login`, `caps`, the
  `flags` bitfield (`logged_in`, `post_login_fetched`), `serverhost`,
  `serverport`, `ip_addr`, `tls`.
- **I/O buffers**: `in`, `out`, `read_in`, `aead_plain` (`struct qbuf`).
- **Extension limits**: `media_max_*`, `history_max_*`, `chat_history_last_msgid`,
  `gif_icons_state`, `gif_icons_probe_timer`.
- **Crypto material (mostly DEAD)**: `sessionkey`, `sklen`, `macalg`,
  `cipheralg`, `cipher_encode_state` / `_decode_state` (union), `cipher_*_key`,
  `cipher_*_keylen`, `cipher_*_type`, `cipher_mode`, `compressalg`,
  `zc_hdrlen` — leftovers from when the HOPE handshake + ciphers lived in C.
  Crypto is now entirely in `hxnet` / `hxcrypto-*`; the live handle is the single
  opaque `hope_aead` (`HxnetHopeAead*`). Most of the rest is vestigial.
- **Legacy list / state-machine plumbing (DEAD)**: `next`, `prev`, `rcv`,
  `real_rcv` — the intrusive multi-conn list (single-conn now) and the retired
  `htlc->rcv` two-phase state machine (deleted in N3).

The dead groups are the key insight: a large fraction of `htlc_conn` is no longer
read by anything live. Culling them is a safe, mechanical precondition that
shrinks the eventual Rust mirror by roughly a third and removes the scariest
fields (raw cipher unions) from the move.

## Phased plan

### Phase E0 — Dead-field cull (precondition, low risk)

Before moving anything, delete what nothing reads. Candidates, each verified by
`grep` for live readers before removal:

- `next` / `prev` (single-conn; the list is never walked).
- `rcv` / `real_rcv` (the `htlc->rcv` state machine is gone; `hx_dispatch_frame`
  routes directly).
- The C cipher state: `cipher_encode_state` / `_decode_state`, `cipher_*_key`,
  `cipher_*_keylen`, `cipher_*_type`, `sessionkey`, `sklen`, `macalg`,
  `cipheralg`, `compressalg`, `cipher_mode`, `zc_hdrlen` — anything only the
  deleted C crypto (`cipher.c` / `hope.c` / …) used. Keep `hope_aead` (live) and
  `aead_plain` only if `network.c::decode` still uses it (audit — the AEAD
  read path may have moved into hxnet already).

This is its own branch/commit, gated on the full suite. It makes every later
phase smaller and is valuable on its own (a leaner struct, less confusion).

### Phase E1 — `htlc_conn` Rust-owned behind a C accessor facade

Mirror the proven `GtkhxSession` / `hxtask` / `HxChatRegistry` playbook: the
struct's storage moves to a Rust crate (`hxconn`, or a module inside `hxnet`),
and C reaches fields through a generated getter/setter FFI
(`hx_conn_uid(htlc)`, `hx_conn_set_uid(htlc, v)`, …).

**Decided (Misha): opaque handle + accessor FFI**, not a `#[repr(C)]` mirror.
C holds an opaque `struct htlc_conn *` and reaches every field through
`hx_conn_get/set_*` shims. This gives the cleanest end state (the layout is
Rust's alone from the start, no duplicated struct to keep in sync); the cost is
more shim boilerplate during the migration, which is mechanical.

**E1a — break the embedding (landed).** `struct htlc_conn` was an embedded
*value* on `session` (`struct htlc_conn htlc;`), and `sess_from_htlc` recovered
the session by `container_of` pointer math — which only works while htlc is
embedded. An opaque Rust-owned struct can't be embedded by value (C would need
its size), so the first concrete step made `session.htlc` a heap pointer
(`struct htlc_conn *htlc`, `g_new0`'d once at startup, owned for the session's
life). `sess_from_htlc` now reads a `session *sess` back-pointer set at
allocation instead of doing container_of (also multi-conn-ready). Two wrinkles
surfaced and were handled: (1) the `cfgvars[]` table bound ICON/NICK to
`&the_session.htlc.icon` at compile time — no longer a constant behind a heap
pointer — so those slots are now NULL in the static table and wired at runtime
by `hx_options_bind_identity()` right after allocation; (2) the Tier-3 connect
harness deliberately embedded its htlc in a `session` to keep container_of
honest, so it now owns explicit storage and re-arms the back-pointer after each
per-test memset. Verified against the full unit/proto suites and the
session-routing Tier-3 tests (real_connect, real_htxf_connect, voice_rejoin_media).

Sequencing within the rest of E1 (the struct is too wide to move atomically):

1. **Opaque handle + allocation ownership.** Rust allocates/frees the struct;
   C holds a `struct htlc_conn *` it no longer dereferences directly. A
   `#[repr(C)]` mirror with `_Static_assert` layout pins (the gtkhx-boxed
   pattern) lets C keep direct field access during the transition while Rust
   owns the definition — so the migration is field-group by field-group, not big
   bang.
2. **Convert readers/writers field-group by field-group**, heaviest files last:
   start with the extension limits and identity scalars (self-contained), then
   the `flags` bitfield, then the I/O buffers (these are the entangled ones —
   `network.c` send path + the bridge both touch `in`/`out`).
3. **Make production opaque** once every production C site goes through
   accessors: `protocol.h` forward-declares `struct htlc_conn` only, and the
   `#[repr(C)]` definition moves to `src/hxconn_layout.h`. Production sees the
   opaque handle + accessors + `hx_conn_new`; it never sees the fields.

**E1c — landed (`claude/hxconn-flip`).** The accessor bodies + lifecycle
(`hx_conn_new` / `_reset` / `_free`) moved into the Rust `hxconn` crate;
`hxconn.c` is deleted. Production allocates the connection via `hx_conn_new` (a
`Box`, never freed — process-lifetime) and reaches every field through the
`hx_conn_*` ABI; `sess_from_htlc` reads through `hx_conn_sess`. `struct
htlc_conn` is opaque in `protocol.h`.

The `#[repr(C)]` mirror is **retired from production** but deliberately kept for
the Tier-2/Tier-3 tests, which stack-allocate a connection (and, in a few
places like `test_hlwrite`, memset it, copy it by value, and read fields
directly) to drive the accessors + parsers. That mirror lives in
`src/hxconn_layout.h`, is included only by test code, and is pinned
`#[repr(C)]`-identical to Rust's `HtlcConn` by a `_Static_assert` (paired with
the crate's `assert!(size_of == HXCONN_SIZEOF)`) — the same
keep-a-pinned-mirror pattern gtkhx-boxed uses. Fully retiring even the test
mirror (heap-allocating tests via `hx_conn_new` + rewriting their direct field
pokes to accessors) is a possible follow-up, deferred as high-churn / low-value
since tests legitimately need to construct and inspect a connection.

Risk: `in`/`out`/`read_in` qbufs are touched by the send framing (`network.c`)
and the bridge staging simultaneously. Those two callers are also the ones Phases
E2/E3 delete — so it may be cleaner to leave the buffers C-side until the
handlers and bridge move, then delete them outright rather than port them.

The buffer removal is scoped in detail in
[network-endgame-buffers.md](network-endgame-buffers.md). `read_in` (dead) and
`out` (send staging) are removed by delete-don't-accessorize. `in` (receive
staging) is the last holdout and is removable the same way — by threading the
received frame as an explicit `(ptr, len)` slice through `hx_dispatch_frame` to
the handlers instead of staging into `htlc->in`. Crucially, that buffer removal
is **independent of the E2 handler-body migration** below: deleting the `in`
field is what unblocks the E1c flip; moving the handlers into Rust is separate
cleanup that can follow.

### Phase E2 — Receive handlers to Rust (delete `rcv.c`)

With `htlc_conn` Rust-owned and the collaborators already Rust, move each handler
family into a Rust `hxrecv` crate (the `hxNNN-recv` crates are the natural homes;
they currently own only the emit half — they absorb the parse + state-mutation
half). The handler signature becomes `(conn: &mut HtlcConn, body: &[u8])` — no
`htlc->in` staging, the body arrives as a slice from the actor.

- `hx_dispatch_frame`'s opcode switch becomes a Rust `match` on `route(type)`
  (the route table is already Rust; only the C `switch` wrapper remains).
- `hx_rcv_task`'s correlation (trans → task → invoke → delete, plus the
  error-suppression + xfer/voice error-dispatch policy) moves to Rust over the
  `hxtask` table. The `rcv` callbacks it invokes become Rust fn pointers /
  enum-dispatched handlers rather than C function pointers.
- Do it family by family (chat, users, msg, news, files/xfers, banner/icons,
  voice, login/lifecycle), each a branch, each gated on its Tier-3 test. `rcv.c`
  shrinks to nothing and is deleted at the end of the phase.

### Phase E3 — Delete the bridge

Once the handlers are Rust and consume `(&[u8])` bodies, `Event::Frame` is
delivered straight to the Rust dispatch inside hxnet — no C staging, no
`gtkhx_proto_pack_header` re-pack (that hop, added in N5, disappears entirely),
no `htlc->in`. The bridge's remaining, genuinely-C-shaped duties are relocated,
not deleted:

- **Connect install** (`hx_bridge_install_orchestrated_{plaintext,hope,tls}`)
  and the **SOCKS proxy lookup** and **TLS-verify trampoline** → these are
  lifecycle glue; they move into hxnet's connect path or a thin Rust
  `hxconn`/lifecycle module.
- **Lifecycle → `GtkhxSession` state signals** (`bridge_on_state_cb` /
  `_shutdown_cb`) → emitted directly from Rust (GtkhxSession is already Rust).

`hxnet_bridge.c` / `.h` deleted. The C side of the network layer is then just the
UI reading `GtkhxSession` signals and the accessor facade for whatever UI code
still needs a connection scalar.

## Recommended order & first increment

1. **E0 dead-field cull** — do this next. Low risk, high clarity, shrinks every
   later phase. (The N5 `pack_header` move already landed as the first bridge
   brick.)
2. **E1** — the long pole; the field-group accessor migration. Bulk of the work.
3. **E2** — handler families to Rust; `rcv.c` deleted incrementally.
4. **E3** — bridge deleted; lifecycle folded into Rust.

## Constraints carried over (do not re-decide)

- **Wire compat with 1.2 / 1.5 / 1.9 is frozen.** Only the implementation moves;
  every phase gates on Tier-3 `real_connect` / `real_htxf_connect` + the proto
  suites staying green (mhxd, Janus, Mobius family).
- **Single-connection scope** holds throughout; do not resurrect `MAX_CONN` /
  the `next`/`prev` list while moving `htlc_conn`. Multi-conn is a later,
  independent UI phase.
- **`hx_rcv_task` error-suppression policy** (login/reconnect errors toasted not
  dialogged; xfer/voice error replies still dispatched to free per-task state) is
  behaviour to preserve verbatim when the correlator moves to Rust.
- **The C-ABI seam discipline**: C hand-declares the `extern` prototypes so
  signature drift is a link error; no cbindgen for these crates.
