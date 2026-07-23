# Network endgame — the `in` / `out` / `read_in` buffer removal

Companion to [network-endgame.md](network-endgame.md). That doc's Phase E1
ends with every `htlc_conn` field behind the `hxconn` accessor seam **except
the three qbuf I/O buffers** — `in`, `out`, `read_in`. This note inventories
those buffers, explains why they are the last thing standing between here and
the **E1c flip** (moving the struct's storage into a Rust `hxconn` crate), and
lays out the removal order.

**Status:** all three buffers are removed. `read_in` (dead) and `out` (send
staging) went first; `in` (receive staging) followed via the frame-slice
threading described below. No qbuf remains on `struct htlc_conn`, so the E1c
flip is unblocked.

## Why the buffers block the flip

Misha's decided end state for `htlc_conn` is an **opaque, Rust-owned handle**:
C holds a `struct htlc_conn *` it never dereferences, reaching every field
through `hx_conn_*` shims. Every other field now works that way. The qbufs did
not — they were touched directly at field granularity (`htlc->in.buf`,
`htlc->in.pos`, `htlc->out.pos`, …).

Two ways to make them opaque:

- **(a) qbuf accessors** — `hx_conn_in_buf(h)`, `hx_conn_in_pos(h)`,
  `hx_conn_out_ptr(h)`, … for all ~155 sites. This is a large amount of
  boilerplate for buffers that are **slated for deletion** the moment the
  receive handlers (E2) and the send-path (below) move to Rust.
- **(b) delete the buffers** as their callers move, then flip. No accessor
  boilerplate; the buffers simply cease to exist C-side.

**(b) is the plan.** The buffers are staging areas for the C receive/send code
that E2/E3 delete; porting them to accessors first would be throwaway work.

## Inventory (as of this note)

Counts are direct field touches in `src/*.c`.

### `read_in` — **DEAD (culled in this branch)**

Zero live readers. It was the pre-orchestrator accumulation buffer for the
length-prefixed frame reader; `hxnet` owns receive accumulation now. Only a
struct declaration in `protocol.h` and one stale comment in `cipher.h`
mentioned it. **Removed** — a free precondition, no behaviour change.

### `out` — send staging — **DONE (removed)**

`hlpack` / `hlpack_chunks` now return a freshly-`g_malloc`'d buffer + length;
`hlwrite` / `hlwrite_chunks` pack into that transient block, hand it to
`hx_bridge_send_frame`, and free it. `htlc_close` lost its out-teardown, and the
`out` field is gone from `struct htlc_conn`. The accumulator was vestigial: each
send drained exactly what it packed, so it never held more than one message
(the orchestrator send is whole-frame — no partial-write retry to buffer
against). Tests that used `htlc->out` as a capture buffer moved to the returned
buffer / `htlc->in`.

### `in` — receive staging — **DONE (removed)**

Removed via the frame-slice threading below. `hxnet_bridge.c::hx_bridge_dispatch_frame`
no longer stages into `htlc->in` via `qbuf_set`; it assembles the frame into a
transient `g_malloc` buffer and passes it to `hx_dispatch_frame` as an explicit
`(frame, frame_len)` slice, freeing it once dispatch returns. Every consumer
that used to read `htlc->in.{buf,pos}` — the `rcv.c` task/body handlers + the
`hx_rcv_task` correlator + the `tsk->rcv` reply callbacks, the `proto_helpers.c`
`hx_*_parse` wrappers, `inline_media_{upload,download}.c`, `tasks.c::task_error`,
and `proto_trace.c` — now reads its threaded slice argument. The three Rust
`hxnews-recv` task callbacks read the same threaded slice; the
`news_recv_bridge.c` `hx_htlc_in_buf` / `hx_htlc_in_pos` accessors that
previewed the seam are deleted. `network.c`'s teardown lost its `htlc->in`
free, and the dead `proto_trace_recv_chunks` walker is gone. The `in` field is
deleted from `struct htlc_conn`; the receive path no longer touches a
per-connection buffer.

The historical inventory (what touched `in` before removal — `rcv.c`,
`proto_helpers.c`, `hxnet_bridge.c`, `inline_media_{upload,download}.c`,
`network.c`, `proto_trace.c`, `news_recv_bridge.c`, `tasks.c`) is preserved in
the git history of this doc if needed.

The buffer removal was deliberately kept **separate from** the E2 handler-body
migration (moving each receive handler into a Rust `hxNNN-recv` crate, the
family-by-family work that eventually deletes `rcv.c`). Removing the field did
not require moving handler bodies to Rust — the same delete-don't-accessorize
move the `out` removal proved, applied to the receive path.

## The `in`-removal shape (frame-slice threading)

Mirror the `out` removal — pass the frame explicitly instead of via a struct
field:

- **`hx_dispatch_frame`** already receives `(type, trans, flag, body_len)`. Add
  the frame pointer: `hx_dispatch_frame(htlc, frame, frame_len, type, trans,
  flag)`, where `frame`/`frame_len` are the whole staged frame the bridge holds.
- **Every body handler** changes from `void hx_rcv_X(struct htlc_conn *htlc)`
  reading `htlc->in.{buf,pos}` to `void hx_rcv_X(struct htlc_conn *htlc, const
  guint8 *frame, gsize frame_len)` reading its arguments. Same for the
  `hx_rcv_task` correlator (it reads `trans` from the frame) and the `tsk->rcv`
  task-reply callbacks (they receive the same slice — thread it through
  `hx_rcv_task` into `tsk->rcv (htlc, frame, frame_len, tsk->ptr, tsk->data)`, or
  stash the current-frame slice on the correlator for the callback to read).
- **`hxnet_bridge.c`** stops the `qbuf_set` staging into `htlc->in` and passes
  its frame buffer straight through.
- **`network.c`'s login-reply replay** likewise hands its bytes to the dispatch
  as a slice.
- **`proto_helpers.c` / `inline_media_*` / `news_recv_bridge.c`** parser wrappers
  take `(frame, frame_len)` params (several already do via the `hx_htlc_in_*`
  accessors — those callers pass the accessor result, which becomes the slice).
- Delete the `in` field from `struct htlc_conn`; the receive path no longer
  touches a per-connection buffer.

Cost: ~20 handler signatures + the bridge + the correlator + the Tier-2 tests
that currently stage into `htlc->in` (the same tests the `out` removal touched —
they'd pass their packed buffer to the handler directly instead of stashing it).
Mechanical, but broad — size it like the `out` removal, not smaller.

## Removal order

1. **`read_in` cull** — done. Precondition, zero risk.
2. **`out` removal** — done. Send path packs into a transient buffer.
3. **`in` removal** — done, via frame-slice threading (above). Was the last E1c
   blocker; independent of the E2 handler-body migration. Gated on unit/proto
   (the Tier-2 receive tests drive it) + Tier-3 receive round-trips (login,
   news_fetch, user_list, chat).
4. **E1c flip** — now unblocked: no qbuf remains on `htlc_conn`, and the field
   accessor seam (`hxconn.c`) covers the rest. The struct's storage moves into
   the Rust `hxconn` crate with the same C ABI (`hxconn.c`'s bodies deleted), and
   the layout is Rust's alone.
5. **E2 proper** — the receive handlers move into Rust family-by-family (see the
   grouping in [network-recv-handler-inventory.md](network-recv-handler-inventory.md)),
   shrinking `rcv.c` to nothing. Independent of the flip; pure cleanup once the
   struct is Rust-owned.

## Next increment — done

**The E1c flip landed** (`claude/hxconn-flip`, see network-endgame.md). With all
three buffers gone and the field accessor seam complete, `struct htlc_conn`
became an opaque, Rust-owned handle: the accessor bodies + `hx_conn_new` /
`_reset` / `_free` lifecycle live in the Rust `hxconn` crate, `hxconn.c` is
deleted, production allocates via `hx_conn_new` and never sees the fields
(`protocol.h` forward-declares only). A pinned `#[repr(C)]` mirror
(`src/hxconn_layout.h`) is kept for the tests that stack-allocate a connection.

> Note on the earlier plan: an earlier revision of this doc said `in` was
> "gated on E2" and would dissolve family-by-family as handlers moved to Rust.
> That conflated the *buffer* removal with the *handler* migration. The buffer
> went first — and had to, since it (not the handler ports) blocked the flip. The
> inventory doc's `htlc->rcv` two-phase-state-machine description is also stale:
> that field was culled and `hx_dispatch_frame` now routes via a
> `switch (hx_recv_route(type))` straight to the body handler.
