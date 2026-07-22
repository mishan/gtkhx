# Network endgame — the `in` / `out` / `read_in` buffer removal

Companion to [network-endgame.md](network-endgame.md). That doc's Phase E1
ends with every `htlc_conn` field behind the `hxconn` accessor seam **except
the three qbuf I/O buffers** — `in`, `out`, `read_in`. This note inventories
those buffers, explains why they are the last thing standing between here and
the **E1c flip** (moving the struct's storage into a Rust `hxconn` crate), and
lays out the removal order.

**Status:** `read_in` (dead) and `out` (send staging) are removed. `in` (receive
staging) is the sole remaining direct-field-access holdout — see the
`in`-removal shape below.

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

### `in` — receive staging — **the remaining blocker**

| File | Touches | Role |
|------|--------:|------|
| `rcv.c` | 32 | every task/body handler reads `htlc->in.buf, htlc->in.pos` and passes it to a Rust `gtkhx_proto_parse_*` / `hl_hdr_decode`; `hx_rcv_dump` writes it to `hx.dump` (debug). |
| `proto_helpers.c` | 13 | the `hx_*_parse` wrappers read `htlc->in.{buf,pos}` and delegate to the crate parsers. |
| `hxnet_bridge.c` | 10 | `hx_bridge_dispatch_frame` **stages** the replayed header+body into `htlc->in` via `qbuf_set` before calling `hx_dispatch_frame`. |
| `inline_media_upload.c` | 6 | reply handlers read `htlc->in.{buf,pos}` into the crate parsers. |
| `network.c` | 5 | login-reply replay staging + trace. |
| `proto_trace.c` | 4 | wire trace reads `htlc->in.{buf,pos,len}`. |
| `news_recv_bridge.c` | 3 | already exposes `hx_htlc_in_buf(htlc)` / `hx_htlc_in_pos(htlc)` accessors for the Rust news receive path — a **partial seam** that previews the end state. |
| `inline_media_download.c` | 3 | reply handlers, as upload. |
| `tasks.c` | 1 | comment only. |

The uniform pattern is **read `(buf, pos)`, feed a Rust parser**, where `(buf,
pos)` is the *whole staged frame* (22-byte header + body) and the parsers skip
the header themselves. `htlc->in` is purely the hand-off region where
`hxnet_bridge.c` deposits a received frame and the C handlers pick it up.

**Two independent efforts are tangled here — keep them apart.**

1. **`in` removal (buffer, E1c-blocking).** Deleting the field does *not* require
   moving handler bodies to Rust. `hxnet_bridge.c` already holds the frame bytes
   before it stages them; the receive path can pass them as an explicit `(ptr,
   len)` slice through `hx_dispatch_frame` to the handlers instead of staging
   into `htlc->in`. Then each handler reads its argument slice rather than
   `htlc->in.{buf,pos}`. This is the exact analog of the `out` removal:
   delete-don't-accessorize, mechanical, and it is what unblocks the **E1c flip**.
2. **Handler-body migration (semantic, E2 proper).** Moving each receive handler
   into a Rust `hxNNN-recv` crate — the family-by-family work that eventually
   deletes `rcv.c`. This is *not* required to remove the `in` field and should
   not be bundled with it.

`hx_rcv_dump` (the `hx.dump` debug writer) and `proto_trace.c` are the only
non-parser readers; both take the same slice once it is threaded through.

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
3. **`in` removal** — next, via frame-slice threading (above). Independent of the
   E2 handler-body migration; this is the last E1c blocker. Gate on unit/proto
   (the Tier-2 receive tests drive it) + Tier-3 receive round-trips (login,
   news_fetch, user_list, chat).
4. **E1c flip** — once `in` is gone, no direct `htlc_conn` field access remains;
   the struct's storage moves into the Rust `hxconn` crate with the same C ABI
   (`hxconn.c`'s bodies deleted), and the layout is Rust's alone.
5. **E2 proper** — the receive handlers move into Rust family-by-family (see the
   grouping in [network-recv-handler-inventory.md](network-recv-handler-inventory.md)),
   shrinking `rcv.c` to nothing. Independent of the flip; pure cleanup once the
   struct is Rust-owned.

## First increment

**Remove `in` via frame-slice threading.** It is the last of the three buffers
and the final direct-field-access holdout before the E1c flip. It does not
require the E2 handler-to-Rust migration — the same delete-don't-accessorize move
the `out` removal proved, applied to the receive path.

> Note on the earlier plan: the previous revision of this doc said `in` was
> "gated on E2" and would dissolve family-by-family as handlers moved to Rust.
> That conflated the *buffer* removal with the *handler* migration. The buffer
> can go first — and should, since it (not the handler ports) is what blocks the
> flip. The inventory doc's `htlc->rcv` two-phase-state-machine description is
> also stale: that field was culled and `hx_dispatch_frame` now routes via a
> `switch (hx_recv_route(type))` straight to the body handler.
