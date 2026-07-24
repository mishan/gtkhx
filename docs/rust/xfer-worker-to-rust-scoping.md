# Xfer worker lifecycle → Rust — scoping

> The third and final HTXF/xfer doc. Its two predecessors are **done**:
>
> - [`htxf-migration-scoping.md`](htxf-migration-scoping.md) moved the **byte
>   transport** to Rust — the subchannel socket, TLS (rustls), and the
>   ChaCha20-Poly1305 AEAD framing all live in `hxnet::htxf` (`HtxfConn`).
> - [`xfers-tokio-scoping.md`](xfers-tokio-scoping.md) moved the **worker
>   scheduling** to Rust — the four transfer workers run on tokio's blocking
>   pool via `gtkhx_bridge_spawn_blocking_with_idle`, with cooperative
>   cancellation (`HtxfAbort`); the last `pthread_create` and `gtkthreads.c`
>   are gone.
>
> Both explicitly left one thing in C: **the worker bodies** (the copy loop,
> the folder mini-protocol, the resource-fork handling) and **`struct
> htxf_conn`** itself. That is what this doc scopes.

## The weave (why this is worth doing)

A single file transfer today threads control back and forth across the FFI
boundary many times, because the worker **body** is C sitting between a Rust
transport underneath it and a Rust codec beside it:

```
rcv_task_file_get         [Rust hxxfer-recv]   parse reply, stamp htxf
  → hx_htxf_set_* (×5)     [C  htxf_accessors]  write the C-owned struct
  → xfer_ready_write       [C  xfers.c]         spawn the worker …
  → spawn_blocking         [Rust hxbridge]      … onto the tokio blocking pool
  → get_thread             [C  xfers.c]         worker body
      → htxf_connect        [Rust hxnet]        open subchannel (+TLS +AEAD)
      → file_recv_one       [C  xfers_recv.c]   the copy loop:
          ├ gtkhx_ffo_*      [Rust hxfiles-xfer] fork-header math   ┐ per
          ├ resource_open    [Rust hxhfs]        local fork I/O      │ file,
          ├ htxf_io_read     [C→Rust htxf_io.c]  read a chunk        │ every
          │   → hxnet_htxf_read [Rust hxnet]     … the actual bytes  │ chunk
          └ post_file_update [C→Rust hxbridge]   progress to main   ┘
```

The insight the map makes obvious: **every substantive thing the worker body
touches is already Rust.** The transport (`hxnet`), the FFO/FILP codec
(`hxfiles-xfer`), and *all* the local resource-fork / HFS-sidecar I/O
(`resource_open` / `resource_len` / `hfsinfo_read/write` / `comment_len` /
`type_creator`, all in `hxhfs`) are Rust crates the worker reaches through
C-ABI shims. The C worker body is **glue between Rust parts**. `htxf_io.c`
(173 LOC) and `htxf_subchannel.c` (68 LOC) are themselves thin shims over
`hxnet`; `htxf_accessors.c` (139 LOC) is the getter/setter seam we just added.

So moving the worker body to Rust is not a rewrite of transfer logic against
new primitives — it is **re-pointing an orchestrator at the Rust crates it
already drives**, so the per-chunk `C→Rust→C→Rust` bounce collapses into a
Rust task calling Rust functions, crossing to C only for the two genuinely
view-side things (feed the preview window, emit progress), which already
marshal to the main thread regardless.

## Current ownership map

| Concern | Owner today | Notes |
|---|---|---|
| Subchannel socket / TLS / AEAD framing | **Rust** `hxnet::htxf` | `HtxfConn`, `hxnet_htxf_{connect,read,write,close}` |
| Cancel token | **Rust** `hxnet` | `HtxfAbort`, `hxnet_htxf_abort_*` |
| Worker scheduling | **Rust** `hxbridge` | tokio blocking pool + main-loop completion |
| Reply parse + stamp | **Rust** `hxxfer-recv` | `rcv_task_file_get` etc. (done) |
| FFO / FILP fork-header codec | **Rust** `hxfiles-xfer` | pure math, no state |
| Local resource fork / HFS sidecar | **Rust** `hxhfs` | `resource_open`, `hfsinfo_*`, `comment_len`, `type_creator` |
| Mac resource decode | **Rust** `hxmacres` | |
| **Worker bodies** (copy loop) | **C** `xfers_recv.c` / `xfers_send.c` | `file_recv_one`, `file_send_one` |
| **Folder mini-protocol** | **C** `xfers.c` / `*_recv/_send.c` | `folder_{recv,send}_all`, FILE_NEXT/SEND/RESUME |
| **`struct htxf_conn`** | **C** `xfers.c` | `g_malloc0`, hand-rolled `g_atomic` refcount, `xfers[]` |
| Preamble pack | **C** `htxf_subchannel.c` | thin over `hl_htxf_hdr_pack` |
| Transport shim | **C** `htxf_io.c` | thin over `hxnet_htxf_*` + `canceled` check |
| Progress / completion → view | **C** `xfers.c` + `hxbridge` | `post_file_update`, `xfer-destroyed` |
| Preview window feed | **C** `xfers_recv.c` | `hx_preview_chunk/set_info/done` (GTK, main thread) |

## The end state

One Rust transfer task per transfer, owning the copy loop and the folder
framing, calling `hxnet` + `hxfiles-xfer` + `hxhfs` **natively** (no
`htxf_io_*` / `gtkhx_ffo_*` / `hx_hfs_*` C-ABI round-trips), and reaching C
only through a narrow view seam: emit progress/`xfer-destroyed` on
`GtkhxSession`, and hand preview chunks to the (main-thread) preview widget.
`struct htxf_conn` is Rust-owned; its cross-thread lifecycle is an `Arc` with
typed atomics instead of the hand-rolled `g_atomic` refcount. When done,
`xfers_recv.c`, `xfers_send.c`, `htxf_io.c`, `htxf_subchannel.c`, and
`htxf_accessors.c` are gone; `xfers.c` shrinks to the `xfers[]` registry + the
C→view glue (and even that can follow).

## Where it lives: `hxnet`, not a new crate

Two different things share the "htxf" name today, and keeping them straight is
what decides placement:

- **`hxnet::htxf::HtxfConn`** — the *transport* handle: the subchannel socket,
  the TLS session, the AEAD framing state. This is exactly what the C struct's
  `void *hx` field points at.
- **`struct htxf_conn`** — the *transfer lifecycle* struct: path, ref, sizes,
  positions, refcount, `canceled`, preview, `filter_argv`, plus that `hx`
  pointer to the transport.

The transfer struct *owns* the transport, and the P1/P2 worker that will drive
it is a tokio task that already needs hxnet's runtime, the `HtxfConn`
transport, and the `HtxfAbort` cancel token. So the transfer struct belongs in
**`hxnet`, alongside the transport** — a new module (e.g. `hxnet::xfer`) beside
the existing `hxnet::htxf`, not a separate crate. Co-locating them means the
whole byte path — transfer ↔ transport ↔ cancel — is native intra-crate Rust
with **zero FFI** once the worker moves, where a separate `hxhtxf` crate would
re-cross a crate boundary into hxnet on every read/write. (The FFO/FILP codec
`hxfiles-xfer` and the HFS I/O `hxhfs` stay their own crates — hxnet depends on
them; they don't depend on it.) The two Rust types get distinct names — keep
`HtxfConn` for the transport; the transfer struct is e.g. `hxnet::xfer::Xfer`,
backing the C `struct htxf_conn` ABI.

## Dependency order: the transfer struct moves first

The struct is the pivot. The worker body can't move to Rust while it mutates a
C-owned `htxf_conn` (the same reason `htlc_conn` blocked the receive handlers
in `network-endgame.md`). And it is the *right* first step on its own merits,
because its concurrency story improves:

- Today the lifecycle is a hand-rolled `g_atomic_int` refcount (xfers[] ref +
  worker ref + one ref per queued progress idle) plus a cross-thread
  `canceled` flag, all in C. Getting an `htxf` field from Rust means the
  accessor seam (`htxf_accessors.c`, added for the receive handlers).
- Rust-owned, this becomes an `Arc<HtxfConn>` whose clones *are* the refs, an
  `AtomicBool`/`AtomicU64` for `canceled`/`total_pos`, and the field access is
  native for the (Rust) worker and recv handlers — C keeps a `hx_htxf_*`
  accessor facade only for the view code that still reads a transfer scalar.

This is the `hxconn` playbook again, with the extra wrinkle (already flagged
in the `htxf_accessors.c` work) that `htxf_conn` is genuinely multi-threaded,
so the mirror needs `Atomic*` fields, not plain ones. That wrinkle is *easier*
to get right in Rust's ownership model than to keep auditing by hand in C.

## What genuinely stays C (or at the seam)

- **Preview window feed.** `hx_preview_*` builds/updates a GTK widget on the
  main thread; the worker already only *feeds* it (bytes marshalled via
  `g_idle_add`). The Rust worker keeps doing exactly that through a thin
  emit — no GTK in the worker, same as today.
- **Progress + completion to the view.** `file-update` / `xfer-destroyed`
  stay `GtkhxSession` signals; the Rust worker emits via the bridge instead
  of `post_file_update`. (The `xfers-tokio` doc's "per-transfer event channel"
  option (a) becomes natural here.)
- **The `xfers[]` registry + tasks-window plumbing** can stay C initially
  (it's view-adjacent) and move later, or become an `HxXferRegistry` in Rust
  like `HxChatRegistry`.
- **`uniquify_path` and a couple of path helpers** are small C utilities the
  loop calls; port alongside or leave as genuine collaborators.

Nothing here is a blocker — they're either already-marshalled view work or
tiny helpers.

## Phased plan

Ordered so the pivot lands first and each phase is independently shippable and
Tier-3-gated.

- **P0 — the transfer struct → Rust (`hxnet::xfer`).** Move `struct htxf_conn`
  into a new `xfer` module in the hxnet crate (beside the `htxf` transport),
  behind the existing `hx_htxf_*` C ABI (the `hxconn` E1c flip): storage +
  `Arc`-based lifecycle in Rust, `canceled`/`total_pos` as atomics,
  `hx_htxf_new/_free` replacing `g_malloc0`/`htxf_unref`. C (xfers.c worker, banner.c, tasks.c,
  the view) reaches fields through the accessor facade unchanged. The
  hand-rolled refcount + `htxf_io_abort_*` bookkeeping collapse into the
  Rust handle. **Keystone; independently valuable (leaner, safer lifecycle).**
- **P1 — single-file copy loop → Rust.** Port `file_recv_one` / `file_send_one`
  into `hxnet::xfer` (the same module as the struct + worker): the Rust worker
  calls `hxnet::htxf` read/write directly, the `hxfiles-xfer` codec, and the
  `hxhfs` fork I/O *natively*. `xfer_ready_write`'s worker entry points at the
  Rust loop. The per-chunk FFI weave is gone for solo files. `htxf_io.c` loses
  its read/write shim (the loop calls `hxnet::htxf` in-crate); the `canceled`
  check moves into the Rust read/write wrapper.
- **P2 — folder mini-protocol → Rust.** Port `folder_recv_all` /
  `folder_send_all` (the FILE_NEXT / FILE_SEND / FILE_RESUME framing + the
  local tree walk + per-file resume). Highest behaviour-risk piece (resume,
  partial transfers, name encoding), so it goes last and leans hardest on the
  Tier-3 folder round-trip. `xfers_recv.c` / `xfers_send.c` delete here.
- **P3 — consolidate banner + delete the shims.** Point `banner.c`'s
  transient-htxf worker at the same Rust path (it already shares
  `htxf_io_*` / `hxnet`), then delete `htxf_io.c`, `htxf_subchannel.c`
  (preamble pack moves into `hxnet::htxf::connect`), and shrink
  `htxf_accessors.c` to whatever the view still needs.

P0 is a precondition for P1–P2. P1 and P2 are the throughput-sensitive
rewrites; P3 is cleanup.

## Risks / open questions

1. **Throughput.** The whole point is removing per-chunk FFI, so this should
   *help*, but the copy loop is the bulk-data path — benchmark a large solo
   file and a deep folder before/after each of P1/P2. The AEAD frames are
   already bulk-sized in `hxnet`.
2. **Folder-protocol fidelity (P2).** FILE_NEXT/SEND/RESUME framing, per-file
   resume (RFLT), and Mac-Roman name encoding must be byte-identical. Wire
   compat with 1.2/1.5 is frozen — gate on the Tier-3 folder round-trip
   against mhxd (and the upload matrix once a permissive server is in CI).
3. **`hxhfs` config threading.** The native `hxhfs` fns take a `Config`
   (dir_char, sidecar mode); today the C wrappers supply a global. The Rust
   worker needs that config handed in (or a process-global in `hxhfs`) — a
   small decision to make in P1.
4. **`htxf_conn` atomics (P0).** The `#[repr(C)]`/opaque mirror must place the
   atomic fields correctly and the C accessors must not race the worker;
   `_Static_assert` layout pins + the existing `test_htxf_cancel` cover it.
5. **Cancel semantics preserved.** Cooperative cancel (`HtxfAbort` + the
   `canceled` re-check) already works; the Rust loop must keep checking at the
   same boundaries. This is a straight carry-over, not a redesign.
6. **Preview / progress marshalling.** The worker must stay GTK-free; the Rust
   loop emits progress + preview chunks through the bridge/idle, same
   contract as today (verified clean in the Phase 3 worker audit).

## Recommendation

Do **P0 first, on its own branch.** It's the keystone (P1–P2 can't move
without it), it's independently valuable (an `Arc`-based lifecycle is leaner
and safer than the hand-rolled `g_atomic` refcount + `htxf_io_abort_*`
bookkeeping), and it's the direct sibling of the `hxconn` / `cfl` moves
already done — the same opaque-handle + accessor-facade shape, plus atomics.
Then reassess P1 (solo copy loop) as the first weave-removal, with P2 (folder)
and P3 (banner + shim deletion) following. Land nothing without the Tier-3
transfer matrix (`file/folder get/put`, TLS variants, cancel/shutdown under
ASan) staying green — wire compat with 1.2/1.5/1.9 is frozen; only the
implementation moves.
