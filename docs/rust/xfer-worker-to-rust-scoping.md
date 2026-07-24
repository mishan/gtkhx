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

## Order: the worker moves first, the struct flip follows

The obvious order is "struct to Rust, then the worker" (the `htlc_conn`
precedent — a C-owned struct blocked the receive handlers). But here the
worker files hold ~180 of the ~230 `htxf->` accesses *and* the gnarly
internal fields (`in` qbuf, `xfer_encode`/`decode` AEAD state, `hx`), and P1
rewrites those files in Rust anyway. Making the struct fully opaque first
would mean accessorizing worker internals that P1 then deletes — throwaway.

So the order is **inverted: move the worker to Rust first, then flip the
struct.** The Rust worker doesn't need the struct to be Rust-owned yet — it
reaches `htxf` scalar state (path, sizes, positions, `canceled`) through the
existing `hx_htxf_*` accessor seam, a handful of calls *per file* rather than
the *per-chunk* `htxf_io` bounce. The byte path — `hxnet::htxf` read/write,
the `hxfiles-xfer` codec, `hxhfs` fork I/O — goes native immediately. That
kills the weave (the whole point) without the struct move as a precondition.

The struct flip then comes *after* the worker is Rust, when it's cheap: the
worker already uses accessors, and the remaining C consumers are the smaller
view set (tasks.c, banner.c, network.c, files.c). At that point the flip is
the `hxconn` E1c shape — storage + an `Arc`-based lifecycle (clones *are* the
refs) + atomic `canceled`/`total_pos` in Rust, behind the same `hx_htxf_*`
facade. The multi-threaded wrinkle (atomics, not plain fields) is *easier* to
get right in Rust's ownership model than to keep auditing `g_atomic` by hand
in C — but it lands last, not first.

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

The worker moves first (weave removal), the struct flip lands last. Each phase
is independently shippable and Tier-3-gated. The Rust worker code lands in a
new `hxnet::xfer` module (beside the `htxf` transport) so its byte path is
in-crate native.

- **W1 — single-file download loop → Rust.** Port `get_thread` + `file_recv_one`
  into `hxnet::xfer`: a Rust worker (spawned via the same
  `gtkhx_bridge_spawn_blocking_with_idle` shim) reads `htxf` scalars through
  the `hx_htxf_*` accessor seam, then runs the copy loop calling `hxnet::htxf`
  read *directly* (in-crate), the `hxfiles-xfer` FFO codec, and `hxhfs` fork
  I/O — all native. Progress/preview marshal to main through a thin bridge
  emit. The per-chunk `htxf_io_read` bounce is gone for solo downloads.
  Smallest, most-testable slice (Tier-3 `file_get`). Start here.
- **W2 — single-file upload loop → Rust.** `put_thread` + `file_send_one` the
  same way (`hxnet::htxf` write, `hxfiles-xfer` pack, `hxhfs` resource read).
  Gated on `file_put` (needs a permissive server).
- **W3 — folder mini-protocol → Rust.** `folder_{recv,send}_all` (the
  FILE_NEXT / FILE_SEND / FILE_RESUME framing + local tree walk + per-file
  resume). Highest behaviour-risk (resume, partial transfers, name encoding),
  so last; leans hardest on the Tier-3 folder round-trip. `xfers_recv.c` /
  `xfers_send.c` / the `htxf_io.c` read/write shim delete here.
- **S0 — flip `struct htxf_conn` → `hxnet::xfer` (Arc + atomics).** Now that
  the worker is Rust, move the struct storage + lifecycle behind the
  `hx_htxf_*` C ABI: `Arc`-based refcount (clones *are* the refs), atomic
  `canceled`/`total_pos`, `hx_htxf_new/_ref/_unref` replacing
  `g_malloc0`/`g_atomic`/`g_free`. The remaining C consumers (view: tasks.c,
  banner.c, network.c, files.c) reach fields through the facade. The
  hand-rolled refcount + `htxf_io_abort_*` bookkeeping collapse into the
  handle. `htxf_accessors.c` shrinks to the view surface.
- **S1 — consolidate banner + delete the shims.** Point `banner.c`'s
  transient-htxf worker at the same Rust path, then delete `htxf_io.c` and
  `htxf_subchannel.c` (preamble pack moves into `hxnet::htxf::connect`).

W1 is the first increment. W1–W3 are the throughput-sensitive rewrites; S0 is
the lifecycle keystone (now cheap); S1 is cleanup.

## W1 — concrete shape (the first increment)

The single-file download is `get_thread` (`xfers.c`, a ~30-line driver:
`htxf_connect` → `file_recv_one(total_size)` → play sound + final progress →
`htxf_io_release`) around `file_recv_one` (`xfers_recv.c`, ~230 lines — the
real work). `file_recv_one`'s structure:

1. read the 40-byte FILP fixed header;
2. `gtkhx_ffo_info_block_len(buf[38], buf[39])` → read the info block;
3. `gtkhx_ffo_parse_filp_info(...)` → type/creator, comment, times, data-fork
   length; `hfsinfo_write(path, fi)` (skipped for previews);
4. data fork: `open(path)` + optional `lseek(data_pos)` + copy loop
   (`rd_wr_recv`), *or* stream through the preview;
5. resource fork (`get_rsrc`): skip for previews; a timed drain for folder
   streams; else read the 16-byte MACR marker, `gtkhx_ffo_fork_len`,
   `resource_open` + optional `lseek(rsrc_pos)` + copy loop;
6. `hfsinfo_write` again.

**Ported to `hxnet::xfer`, each dependency goes native or to a narrow seam:**

- **Transport (hot path).** `htxf_io_read` → call `hxnet::htxf`'s read
  *in-crate* on the `HtxfConn` the worker gets from `hx_htxf_hx(htxf)`. The
  `canceled` re-check + errno mapping that `htxf_io.c` did move into a small
  Rust read wrapper.
- **Codec.** `gtkhx_ffo_*` → `hxfiles-xfer` native calls.
- **Local forks.** `hfsinfo_write` / `resource_open` → `hxhfs` native;
  `open`/`write`/`lseek`/`fsync`/`close` → `std::fs`.
- **Preview + progress (view seam).** `hx_preview_chunk/set_info/done` and the
  per-chunk progress emit stay FFI/bridge calls (they already marshal to
  main); the worker stays GTK-free.

**Accessors W1 needs** (extend `htxf_accessors.c`; getters unless noted):
`hx` (the `HtxfConn*`), `total_pos` (get + set — bumped per chunk; keep a Rust
local and push at progress points), `total_size`, `data_pos`, `rsrc_pos`,
`opt.large`, `opt.folder`, `canceled`. (`path`, `opt.preview`, `preview`,
`data_size` already exist.) These are per-file, not per-chunk, except the
`total_pos` push + `canceled` check, which sit alongside the progress emit that
already crosses per chunk.

**The one real decision — `hxhfs` config threading (risk #3).** The native
`hxhfs` fns (`hfsinfo_write`, `resource_open`) take a `Config` (dir_char +
sidecar mode); today the C wrappers supply a process global. The Rust worker
needs that config — simplest is a `hxhfs` process-global set once at startup
(mirroring the C global), read by the worker. Decide this first in W1.

**Gate:** Tier-3 `file_get` (solo download, plain + TLS) is the W1 regression
guard; it already drives `xfer_ready_write` → the worker → progress →
completion end-to-end.

## Risks / open questions

1. **Throughput.** The whole point is removing per-chunk FFI, so this should
   *help*, but the copy loop is the bulk-data path — benchmark a large solo
   file and a deep folder before/after each of W1–W3. The AEAD frames are
   already bulk-sized in `hxnet`.
2. **Folder-protocol fidelity (W3).** FILE_NEXT/SEND/RESUME framing, per-file
   resume (RFLT), and Mac-Roman name encoding must be byte-identical. Wire
   compat with 1.2/1.5 is frozen — gate on the Tier-3 folder round-trip
   against mhxd (and the upload matrix once a permissive server is in CI).
3. **`hxhfs` config threading.** The native `hxhfs` fns take a `Config`
   (dir_char, sidecar mode); today the C wrappers supply a global. The Rust
   worker needs that config handed in (or a process-global in `hxhfs`) — a
   small decision to make in W1.
4. **Worker reaches a C-owned struct (W1–W3).** Until S0 the worker (Rust)
   reads/writes `htxf` through the `hx_htxf_*` accessor seam — a few calls per
   file, not per chunk, so no hot-path FFI. It does mean the accessor surface
   must cover every field the worker touches (built out incrementally per
   slice), and the `canceled`/`total_pos` accessors must stay `g_atomic` on
   the C side until S0 makes them Rust atomics.
5. **Cancel semantics preserved.** Cooperative cancel (`HtxfAbort` + the
   `canceled` re-check) already works; the Rust loop must keep checking at the
   same boundaries. This is a straight carry-over, not a redesign.
6. **Preview / progress marshalling.** The worker must stay GTK-free; the Rust
   loop emits progress + preview chunks through the bridge/idle, same
   contract as today (verified clean in the Phase 3 worker audit).

## Recommendation

Do **W1 (single-file download loop) first, on its own branch.** It's the
smallest slice that removes the per-chunk weave end-to-end and it's directly
gated by the Tier-3 `file_get` test. The Rust worker reaches the still-C-owned
`htxf` through the accessor seam (per-file, not per-chunk) and calls
`hxnet::htxf` / `hxfiles-xfer` / `hxhfs` natively on the byte path — proving
the shape before W2 (upload) and W3 (folder). The `struct htxf_conn` flip (S0
— `Arc` + atomics behind the `hx_htxf_*` facade, the direct sibling of the
`hxconn` / `cfl` moves) lands *after* the worker is Rust, when it's cheap.
Land nothing without the Tier-3 transfer matrix (`file/folder get/put`, TLS
variants, cancel/shutdown under ASan) staying green — wire compat with
1.2/1.5/1.9 is frozen; only the implementation moves.
