# xfers.c → Rust — scoping

> Moving `src/xfers.c` (~1038 lines, the GTK file-transfer **worker shell**) into
> Rust. The transfer byte-loops (`hxnet::xfer`) and the `struct htxf_conn`
> storage + refcount/cancel lifecycle (`hxnet::xfer_handle`, the `hx_htxf_*` ABI)
> are already Rust from the W1–W3 / S0–S1 migration. This is the remaining shell:
> the `xfers[]` list, the worker/completion dispatch, `xfer_go`'s wire-request
> build, construction, param marshaling, the four worker threads, local-path
> helpers, and the file-update / xfer-destroyed signals.
>
> Successor concern to the xfer-worker migration in `docs/rust/ROADMAP.md`.

## Decided

- **Home: `hxhandlers`, a new `src/xfer/` module** (Misha). hxhandlers already
  owns the *receive* half of the transfer domain (`recv/xfer.rs` — the
  `rcv_task_file_*` handlers that already call `xfer_ready_write` / `xfer_new`
  over the C ABI), so co-locating the send/lifecycle shell keeps the whole
  domain in one crate and turns those extern calls into native ones. **Not
  hxnet**: hxnet is a strict *leaf* (references no C symbols), and the shell
  calls a pile of them (`htxf_connect`, `play_sound`, `hx_preview_*`,
  `path_to_hldir`, `task_new`, `hlwrite_chunks`, `gtkhx_bridge_*`). **Not a new
  crate**: against the consolidation effort.

## Open decision — how the shell reaches `struct htxf_conn` fields

`HtxfHandle` (hxnet::xfer_handle) is a `#[repr(C)]` mirror of `struct htxf_conn`,
**private fields**, layout pinned by `tests/unit/test_htxf_layout.c`. Today C
reads most fields *directly* (path, remotename, remotedir, remotepath, htlc,
type, opt, hx, preview, srv_data_size, data/rsrc_pos/size…) through the visible
`struct htxf_conn` declaration, and only the lifecycle trio
(refcount/canceled/total_pos) goes through `hx_htxf_*`. The shell touches ~20
fields heavily. Two ways to reach them from hxhandlers:

- **A — native (recommended).** hxhandlers gains an `hxnet` dep; `HtxfHandle`'s
  fields become `pub` (or the crate adds native getter/setter methods). The
  shell reads/writes fields directly in Rust — no `Rust → C-ABI → Rust` accessor
  bounce (the thing we've been removing). Cost: hxhandlers now compiles against
  hxnet (already linked in the binary; only grows hxhandlers' compile graph +
  `cargo test -p hxhandlers`, which pulls tokio/rustls). hxnet does not depend on
  hxhandlers, so the edge is acyclic.
- **B — C-ABI accessor seam.** Keep `hx_htxf_*`; add the ~15 missing
  getters/setters (path, remotename, opt-bit accessors, htlc, hx, preview, …).
  hxhandlers `extern`s them. No new crate coupling, but it's exactly the
  Rust→C-ABI→Rust bounce we prefer to avoid, plus a lot of accessor boilerplate.

**Recommendation: A.** It matches the "avoid Rust→C→Rust" direction and the
shell is intrinsically part of the xfer domain hxnet already owns. If the
tokio-in-hxhandlers-tests cost bites, fall back to B for the hottest fields.

## The `xfers[]` registry

`int nxfers` + `struct htxf_conn **xfers` — a main-thread-only dynamic array of
live transfers. Only the main thread mutates it (add in `xfer_init`, remove in
`xfer_remove_from_list`); workers touch only *their own* htxf, and only its
atomic fields (`total_pos`, `canceled`) — never the list. So the Rust home is a
**`thread_local!` `Vec<*mut HtxfHandle>`** (no locking needed), with the array
ops (`xfer_up`/`_down`/`_num`/`htxf_with_ref`/`remove`/`delete_all`/
`tasks_update`) as methods. Raw pointers (not `Box`) because ownership is the
refcount, not the list slot — the list holds one ref, dropped on removal via
`hx_htxf_unref`.

## ABI surface to preserve (hand-declared C `extern`s; drift = link error)

Called from **C**: `xfer_new` (files.c ×2, files_ops.c, files_remote_provider.c),
`xfer_new_folder` (files.c ×2), `xfer_up`/`_down`/`_num` (tasks.c), `xfer_go`
(tasks.c queue-restart), `xfer_delete` (tasks.c cancel), `xfers_delete_all`
(gtkhx.c shutdown), `xfer_tasks_update` (tasks.c), `htxf_with_ref` (rcv.c QUEUE).
Called from **Rust** (hxhandlers recv/xfer.rs): `xfer_ready_write` (the announce
tail of every reply handler), `xfer_delete`, `xfer_go_timer` (retry). These
become native intra-crate calls once the shell is in hxhandlers.

## Collaborators

- **Stay C (extern leaf calls):** `htxf_connect` (network.c — opens the HTXF
  subchannel; itself a thin wrapper over `hxnet_htxf_connect` + abort-arm),
  `play_sound` (sound.c), `hx_preview_*` (preview.c — GTK preview window),
  `path_to_hldir` / `dirmask` / `dirchar_fix` / `dir_char` (path utils),
  `uniquify_path` (uniquify_path.c), `resource_len` / `hx_file_size` (hxhfs, but
  reached as C today), `hx_printf_prefix`.
- **Native Rust:** `hxnet_xfer_file_recv_one`/`_send_one`/`folder_recv_all`/
  `_send_all` + `HxnetXferParams`/`HxnetFolderParams` (hxnet::xfer), `hx_htxf_*`
  (hxnet::xfer_handle → native under option A), `gtkhx_session_emit_file_update`/
  `_xfer_destroyed`/`_xfer_queue` (gtkhx-core), `build_file_get_chunks`/
  `build_file_put_chunks` (hotline-proto), `gtkhx_text_for_wire` (hxtext),
  `gtkhx_bridge_spawn_blocking_with_idle`/`gtkhx_bridge_post_to_main` (hxbridge —
  extern today; native if we add the dep, otherwise extern), `task_new` /
  `hlwrite_chunks` (hxtask — extern, as everywhere).

## Threading & marshaling (preserve verbatim)

- **main thread:** `xfer_new`→`xfer_init` (registry add + initial file-update) +
  `xfer_go` (wire request); `xfer_ready_write` (spawn); reorder/lookup; the
  completion tail.
- **tokio blocking pool:** `xfer_worker_entry` → dispatch on `opt.folder`×`type`
  → `get`/`put`/`folder_get`/`folder_put`_thread → `htxf_connect` +
  `hxnet_xfer_*` + `play_sound` + progress. Never touches the list or GTK.
- **marshaling:** progress → `post_file_update` → `gtkhx_bridge_post_to_main`
  (`g_main_context_invoke`) → `fu_dispatch` (emits file-update, drops a ref).
  Completion → `gtkhx_bridge_spawn_blocking_with_idle`'s completion cb
  (`xfer_completion_entry`, main) → `g_idle_add(xfer_cleanup_dispatch,
  DEFAULT_IDLE)`. **The idle-priority ordering is load-bearing:** cleanup runs at
  `G_PRIORITY_DEFAULT_IDLE`, *below* the `DEFAULT`-priority file-update idles, so
  every progress update drains before the transfer is unlinked. Reproduce
  exactly (the completion-hang fix depended on this).
- **refcount:** list = 1 ref, worker = 1 ref (taken in `xfer_ready_write`), each
  pending file-update idle = 1 ref. UAF-invisible-to-tests territory — port the
  ref/unref pairings 1:1, don't "improve".

## Phases (dependency-ordered; one branch each; each gated on Tier-3)

- **Y1 — registry. ✅ SHIPPED.** The `thread_local` `Vec<*mut HtxfHandle>` +
  `xfer_up`/`_down`/`_num`/`htxf_with_ref`/`xfer_remove_from_list`/
  `xfers_delete_all`/`xfer_tasks_update` (+ new `xfer_registry_add`/`xfer_count`
  replacing the `nxfers`/`xfers[]` globals, and `hx_htxf_in_list` folded in from
  htxf_accessors.c), behind the same C ABI. `remove_from_list` emits
  xfer-destroyed + kicks the next queued `xfer_go` (still C, extern). Resolved
  the field-access decision: **option A** — hxhandlers now deps hxnet (acyclic),
  the plain `HtxfHandle` fields the shell reads are `pub`, atomics stay behind
  `hx_htxf_*`.
- **Y2 — construction + lifecycle marshal. ✅ SHIPPED.** `xfer_init`/`xfer_new`/
  `xfer_new_folder` + `post_file_update`/`fu_dispatch` +
  `xfer_completion_entry`/`xfer_cleanup_dispatch`. **`xfer_new` is native now →
  unblocks `hx_cfl_complete_entry`.** `xfer_new` still calls C `xfer_go` until
  Y4; `htxf_ref` + `xfer_close_channel` + `htxf_destructor` stay C until Y5
  (`htxf_destructor` un-static'd so xfer_init can register it). Two principled
  C-ABI exceptions: the `opt` bitfield setters (C owns the bit layout) and the
  `gtkhx_prefs.queuedl` read (`hx_prefs_queuedl` accessor).
- **Y3 — worker dispatch + params. ✅ SHIPPED.** `xfer_ready_write` (takes the
  worker ref + spawns) / `xfer_worker_entry` (dispatch on `opt.folder`×`type`) +
  the four `*_thread`s (`get`/`folder_get`/`put`/`folder_put`) +
  `xfer_recv_params`/`_send_params`/`_folder_params` (native fill of
  `HxnetXferParams`/`HxnetFolderParams`) + `xfer_progress_bump`. Pulled
  `xfer_close_channel` forward from Y5 (the workers call it 4× — keeping it C
  would have been a Rust→C→Rust bounce over `hxnet_htxf_close`); the still-C
  `htxf_destructor` now externs it. `htxf_ref` deleted (`xfer_ready_write` uses
  native `hx_htxf_ref`). New opt-bit getters `hx_htxf_opt_folder`/`_large`
  (htxf_accessors.c; the `opt` bitfield stays C-owned). Remaining C in xfers.c:
  `xfer_go`/`xfer_go_timer` + local-path helpers (Y4) + `htxf_destructor` (Y5).
- **Y4 — wire build + path. ✅ SHIPPED.** `xfer_go`/`xfer_go_timer` +
  `uniquify_local_path`/`local_path_exists_adapter`. The FILE_GET/PUT chunk build
  (native `hotline_proto::build::build_file_{get,put}_chunks` + `ClientHdr`
  opcodes), the download resume-vs-rename decision, and task registration + frame
  write through **native** `hxtask::task_new` + `hxtask::send::hlwrite_chunks`
  (added an hxhandlers→hxtask dep; acyclic). Fixed hxtask's `RcvTaskFn` alias,
  which was a stale 3-arg `(htlc, ptr, data)` shape — the real dispatch (rcv.c) is
  5-arg `(htlc, frame, frame_len, ptr, data)` — so the 5-arg reply handlers now
  register natively without the fn-pointer cast the news/chat senders still use.
  `hx_conn_has_cap` is native (gtkhx-core); `resource_len` native (hxhfs);
  `path_to_hldir` / `exists_remote` / `uniquify_path` / `hx_file_size` stay C
  externs. Remaining C in xfers.c: just `htxf_destructor` (Y5).
- **Resume RFLT fix (follow-up to Y4).** The Y4 port first reproduced the
  original C resume `RFLT` blob byte-for-byte, which was malformed: the string
  literal's continuation-line indentation leaked 26 leading spaces in, shoving
  the RFLT magic to `[26]` and the DATA/MACR fork tags past byte 74. mhxd happens
  to read the two fork offsets at fixed positions `[46]`/`[62]` and ignores the
  rest, so resume still worked against it — but a spec-strict server that parses
  the fork list would choke. Replaced with a well-formed `build_resume_rflt`
  (RFLT magic at `[0]`, version 1, fork count 2, DATA/MACR tags, offsets at
  `[46]`/`[62]` — byte-identical to mhxd's own client). Guarded by an hxhandlers
  unit test on the record layout + a Tier-3 `file_resume` test (resume a
  partial download vs mhxd, assert the completed file is byte-exact).
- **Y5 — finalize.** `htxf_destructor` (preview unref + channel close) +
  `xfer_close_channel`; register the destructor from Rust. Delete `xfers.c` +
  `xfers.h` (keep the ABI decls where C still calls in). `htxf_destructor` may
  stay a tiny C shim if the GTK preview teardown is cleaner there.

## Hard constraints (do not re-decide)

- **Wire compat 1.2/1.5/1.9 frozen** — only the implementation moves. Every phase
  gates on Tier-3 `file_get`/`file_put`/`folder_roundtrip` + `real_htxf_connect`
  vs mhxd/Janus, plus the unit/proto suites.
- **Refcount/UAF discipline** — reason about every ref/unref; the completion
  ordering (idle priority) and the worker/list/idle ref triad are exact.
- **Single-connection scope** holds; don't resurrect multi-conn plumbing.
- **Preserve the queue auto-start + cancel semantics** — `remove_from_list`
  starting the next queued transfer, `xfer_delete` aborting the socket via the
  `HtxfAbort` token, `xfers_delete_all`'s shutdown cancel.
