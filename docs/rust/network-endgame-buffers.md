# Network endgame — the `in` / `out` / `read_in` buffer removal

Companion to [network-endgame.md](network-endgame.md). That doc's Phase E1
ends with every `htlc_conn` field behind the `hxconn` accessor seam **except
the three qbuf I/O buffers** — `in`, `out`, `read_in`. This note inventories
those buffers, explains why they are the last thing standing between here and
the **E1c flip** (moving the struct's storage into a Rust `hxconn` crate), and
lays out the removal order.

## Why the buffers block the flip

Misha's decided end state for `htlc_conn` is an **opaque, Rust-owned handle**:
C holds a `struct htlc_conn *` it never dereferences, reaching every field
through `hx_conn_*` shims. Every other field now works that way. The three
qbufs do not — they are touched directly at ~155 sites as `htlc->in.buf`,
`htlc->in.pos`, `htlc->out.pos`, etc.

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

### `out` — send staging, **2 files, self-contained**

| File | Touches | Role |
|------|--------:|------|
| `network.c` | 24 | `hlwrite` / `hx_send_login_reply_flush` stage packed frames into `htlc->out`, then drain via `hx_bridge_send_frame(&htlc->out.buf[off], len)`; `htlc_close` frees the buffer. |
| `proto_helpers.c` | 1 | `hlpack_chunks` grows `htlc->out` and writes the packed frame into it (`q = &htlc->out`). |

The whole `out` lifecycle is: `hlpack_chunks` appends a packed frame → `hlwrite`
hands the new bytes to `hx_bridge_send_frame` → on success the consumed span is
dropped. There is no longer any partial-write retry against a socket (the
orchestrator's `hx_bridge_send_frame` takes the whole frame); the qbuf is just a
scratch region between pack and hand-off.

**`out` is removable independently of E2/E3.** It only involves the send path in
two files. The shape: have `hlpack_chunks` pack into a **transient buffer** (a
`GByteArray` or a stack/`g_malloc` buffer sized by `compress_encode_bufsize`-style
math) that `hlwrite` hands straight to `hx_bridge_send_frame`, then frees. No
`htlc->out` field, no `pos`/`len` bookkeeping, no free-on-close. This is a
tractable, self-contained increment that removes one of the three buffers well
ahead of the receive-handler migration.

### `in` — receive staging, **9 files, gated on E2**

| File | Touches | Role |
|------|--------:|------|
| `rcv.c` | 32 | every task/body handler reads `htlc->in.buf, htlc->in.pos` and passes it to a Rust `gtkhx_proto_parse_*` / `hl_hdr_decode`; `hx_rcv_dump` writes it to `hx.dump` (debug). |
| `proto_helpers.c` | 13 | the `hx_*_parse` wrappers read `htlc->in.{buf,pos}` and delegate to the crate parsers. |
| `hxnet_bridge.c` | 10 | `hx_bridge_dispatch_frame` **stages** the replayed header+body into `htlc->in` via `qbuf_set` before invoking the dispatch. |
| `inline_media_upload.c` | 6 | reply handlers read `htlc->in.{buf,pos}` into the crate parsers. |
| `network.c` | 5 | login-reply replay staging + trace. |
| `proto_trace.c` | 4 | wire trace reads `htlc->in.{buf,pos,len}`. |
| `news_recv_bridge.c` | 3 | already exposes `hx_htlc_in_buf(htlc)` / `hx_htlc_in_pos(htlc)` accessors for the Rust news receive path — a **partial seam** that previews the end state. |
| `inline_media_download.c` | 3 | reply handlers, as upload. |
| `tasks.c` | 1 | comment only. |

The uniform pattern is **read `(buf, pos)`, feed a Rust parser**. `htlc->in` is
the hand-off region where `hxnet_bridge.c` deposits a received frame's bytes and
the C handlers pick them up. When E2 moves a handler family into Rust, that
family's handler takes the body as a `&[u8]` slice straight from the actor —
no `htlc->in` staging, no re-pack. So `in` disappears **family by family** as E2
proceeds, and the field is deleted when the last C reader (and the
`hxnet_bridge.c` stager) is gone in E3.

`hx_rcv_dump` (the `hx.dump` debug writer) and `proto_trace.c` are the only
non-parser readers; both are diagnostics that either move with their handler or
switch to the slice the Rust dispatch already has.

## Removal order

1. **`read_in` cull** — done in this branch. Precondition, zero risk.
2. **`out` removal** — next, self-contained (`network.c` + `proto_helpers.c`):
   pack into a transient buffer handed to `hx_bridge_send_frame`; delete the
   `out` field, its `pos`/`len` bookkeeping, and the free-on-close. Gated on the
   full unit/proto suite (the Tier-2 `hlpack` tests drive this path directly)
   plus a Tier-3 send-path check (any RPC round-trip: login, news_fetch).
3. **`in` removal** — folded into **E2** (receive handlers → Rust). Each handler
   family that moves stops reading `htlc->in`; the `hxnet_bridge.c` stager and
   the field itself are deleted in **E3** when the last C reader is gone. The
   `news_recv_bridge.c` `hx_htlc_in_*` accessors are the template for any
   interim reader that must stay C-side across a family boundary.
4. **E1c flip** — once all three buffers are gone, no direct `htlc_conn` field
   access remains; the struct's storage moves into the Rust `hxconn` crate with
   the same C ABI (`hxconn.c`'s bodies deleted), and the layout is Rust's alone.

## First increment

**Remove `out`.** It is the only buffer removable without the E2 receive-handler
migration, touches just two files, and its send-path semantics are already a
thin pack→hand-off with no socket-level retry to preserve. Doing it now shrinks
the struct by one qbuf and proves the delete-don't-port approach before the
larger `in`/E2 work.
