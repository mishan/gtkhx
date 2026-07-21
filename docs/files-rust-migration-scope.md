# Files subsystem — C→Rust migration scope

> Scoping document. No code changes here — this maps the components, what's
> already in Rust, what's worth moving, and a phased plan with risk/effort.

## TL;DR

The "files stuff" is ~9.8k LOC of C across `src/files_*.c`, `src/htxf_*.c`,
and `src/xfers.c`. But a lot of the *protocol* is **already in Rust**:
`hotline-proto` builds/parses nearly every file/folder opcode, `hxnet`
owns the HTXF subchannel transport + AEAD, and `hxbridge` owns the
worker-thread pool. So this is not a greenfield port.

The genuine remaining C targets, in priority order:

1. **`xfers.c` (1837 LOC) — the transfer workers.** FFO/HFS frame
   handling, local file I/O, and the folder-walk state machines. This is
   the biggest, highest-value target and has a strong integration-test
   net already.
2. **`filelist_walker.c` + the pure helpers in `files.c`/`files_entry.c`**
   (icon/type formatters, preamble packing, path helpers). Small,
   pure, low-risk — good first movers to establish the crate + FFI shape.
3. **The remote provider's model** (`files_remote_provider.c`) — the
   listing model + reply handling could become an `hxfiles-model` crate
   like `hxchat-model` / `hxnews-model`.

The **UI stays in C**: `files_browser.c`, `files_panel.c`,
`files_complete.c`, and the local provider (`files_local_provider.c`,
GIO-based). GTK widget code is deliberately not a Rust target (matches
the existing MVC boundary: view = C/GTK, model/protocol/IO = Rust).

## Current state: layer map

| Layer | Files (LOC) | Verdict |
|-------|-------------|---------|
| **VIEW (GTK)** | files_browser.c (2323), files_panel.c (1674), files_complete.c (694), files_entry.c view parts (181) | **Keep in C** |
| **LOCAL I/O (GIO)** | files_local_provider.c (506) | **Keep in C** (GIO, low churn) |
| **PROTOCOL / orchestration** | files.c (957), files_ops.c (389), files_provider.c (185), files_remote_provider.c (746), filelist_walker.c (65) | **Partly move** — wire bits already Rust; move the model + pure helpers |
| **TRANSFER I/O** | xfers.c (1837), htxf_io.c (173), htxf_subchannel.c (68) | **Move the workers** — htxf_io/subchannel are already thin Rust shims |

## Component inventory

| File | LOC | Responsibility | Move? | Risk |
|------|-----|----------------|-------|------|
| `xfers.c` | 1837 | put/get/folder_put/folder_get worker state machines; FFO framing; HFS forks; cancellation; refcount lifecycle | **Yes → Rust** | High |
| `files.c` | 957 | file-info dialog, icon/type formatters, `hx_file_*` command orchestration (build via Rust proto + `hlwrite_chunks`) | **Split**: helpers→Rust; dialog/orchestration→C | Med |
| `files_remote_provider.c` | 746 | remote listing provider; FILE_LIST RPC; reply routing from rcv.c; timeout watchdog | **Split**: model→Rust; rcv routing + watchdog→C | Med |
| `files_ops.c` | 389 | cross-panel copy/move dispatch; access-bit checks; local↔local GIO recursive copy | Keep in C (thin glue) | Low |
| `files_provider.c` | 185 | `HxFilesProvider` GInterface + signals | Keep in C (GObject interface) | Low |
| `files_entry.c` | 181 | file-row GObject; size/modified formatters | **Split**: formatters→Rust; GObject→C | Low |
| `files_complete.c` | 694 | local path-completion popover | Keep in C (pure GTK) | — |
| `files_local_provider.c` | 506 | GIO directory ops | Keep in C | — |
| `files_panel.c` | 1674 | single-panel widget (GtkColumnView, inline rename, icon cache) | Keep in C | — |
| `files_browser.c` | 2323 | two-panel window; action orchestration; DnD | Keep in C | — |
| `filelist_walker.c` | 65 | walk packed FILE_LIST bytes, callback per entry (already calls Rust decoder) | **Yes → Rust** | Low |
| `htxf_io.c` | 173 | thin shim over hxnet Rust HTXF (errno mapping, abort) | Mostly Rust already; fold into workers | Low |
| `htxf_subchannel.c` | 68 | HTXF preamble packing (16/24 byte) | **Yes → Rust** (pure byte packing) | Low |

## What's already in Rust (don't re-do)

- **`hotline-proto`** — builds/parses the file opcodes:
  `build_file_{list,delete,mkdir,getinfo,setinfo,move,symlink,getfolder,putfolder}_chunks`
  and `parse_file_{get,put,getinfo,list_entry}_reply`,
  `parse_folder_{get,put}_reply`. `files.c` already calls these via FFI +
  `hlwrite_chunks`. **No wire byte-swap remains in the C files layer.**
- **`hxnet`** — HTXF subchannel open/connect/read/write/close, the
  ChaCha20-Poly1305 AEAD framing, TLS, and the abort token. `htxf_io.c`
  is a ~170-line errno-mapping shim over it.
- **`hxbridge`** — `gtkhx_bridge_spawn_blocking_with_idle` runs the
  transfer workers on a tokio blocking pool and dispatches completion
  back to the GLib main loop.
- **Crate pattern to follow**: subsystems are split into
  `hx<name>-model` / `-recv` / `-send` crates (see `hxchat-*`,
  `hxnews-*`, `hxmember-model`). There is **no `hxfiles-*` crate yet** —
  the migration creates it.

## Proposed target architecture

Follow the established per-subsystem split:

- **`hxfiles-model`** — the file-entry / listing model (name, type,
  size, modified, icon id, fork sizes). Boxed types via `gtkhx-boxed`
  if they cross the signal boundary. Replaces the model half of
  `files_entry.c` + `files_remote_provider.c`.
- **`hxfiles-xfer`** (or extend `hxnet`) — the transfer workers:
  put/get/folder_put/folder_get as async/blocking tokio tasks that own
  the FFO frame codec, HFS fork logic, and local file I/O. Replaces the
  bulk of `xfers.c`, `htxf_io.c`, `htxf_subchannel.c`. Cancellation +
  progress cross back to C via the existing FFI shapes (atomic flag,
  `file_update` idle).
- **Pure helpers** land in the relevant crate (`hxfiles-model` for
  icon/type/size formatters; `hotline-proto` for preamble packing +
  the filelist walk, which already lives next to the entry decoder).

The C side keeps: `files_provider.c` interface, both providers'
GObject/GIO/rcv-routing shells, `files_panel.c`, `files_browser.c`,
`files_complete.c`, `files_ops.c`.

## Phased plan

Each phase is independently shippable, behind the existing test net, on
its own `claude/*` branch.

### Phase F1 — pure helpers (warm-up, low risk)
- Move to Rust: `hx_htxf_subchannel_pack_preamble` (byte packing), the
  FILE_LIST walk loop in `filelist_walker.c`, `icon_of_ftype_and_name`,
  `kind_of_ftype`, and the size/modified formatters.
- Keeps the C signatures; swaps bodies for FFI calls.
- **Test:** existing `test_htxf_hdr`, `test_filelist_walker` cover most;
  add Tier-1 unit tests for the formatters.
- **Value:** establishes `hxfiles-model` + the FFI shape; removes ~200
  LOC of fiddly C.

### Phase F2 — the transfer workers (the big one)
- Port `put_thread`, `get_thread`, `folder_put_thread`,
  `folder_get_thread` into `hxfiles-xfer` (tokio), owning the FFO frame
  codec, HFS fork extraction (`hfs_m_to_htime`, fork headers), the
  folder DFS walk, and local file read/write.
- `htxf_io.c` / `htxf_subchannel.c` fold into the Rust worker; the C
  `xfers.c` shrinks to the GObject/xfers-list/refcount + `file_update`
  signal shell (or that moves too).
- **Test:** strong existing net — `test_file_get/put`, `test_folder_get/
  put`, `test_folder_xfer`, `test_large_file`, `test_htxf_cancel`,
  `test_xfer_queue`, `test_real_htxf_connect`. Run against mhxd/Janus.
- **Risk:** High — this is the core. But it's pure logic + IO (no GTK),
  already on the tokio pool, and covered by integration tests. Do it in
  slices: single-file get, then put, then folder get, then folder put.
- **Value:** the bulk of the win; removes ~1800 LOC of frame/fork C.

### Phase F3 — remote provider model
- Extract the listing model + reply handling from
  `files_remote_provider.c` into `hxfiles-model` (like `hxchat-model`).
- Keep the rcv.c signal routing + timeout watchdog in C (event-loop
  specific), calling into the Rust model.
- **Test:** `test_file_list`, `test_file_list_subdir`, `test_file_info`.

### Phase F4 (optional) — provider trait in Rust
- Only if F1–F3 prove out. Consider a `glib::subclass` implementation of
  `HxFilesProvider`. Lower priority; the GObject interface is a fine
  boundary as-is.

## Test safety net

Already present (this is what makes F2 tractable):

- **unit:** `test_filelist_walker`, `test_folder_xfer`, `test_large_file`,
  `test_xfer_queue`, `test_htxf_cancel`, `test_htxf_hdr`
- **proto:** file/folder reply + list-entry parsers (in `hotline-proto`)
- **integration (mhxd/Janus):** `test_file_get`, `test_file_put`,
  `test_file_info`, `test_file_list`, `test_file_list_subdir`,
  `test_folder_get`, `test_folder_put`, `test_real_htxf_connect`

Gaps to add: Tier-1 unit tests for the icon/type/size formatters (F1);
a Rust-side unit test for the FFO frame codec + HFS fork math (F2) so
regressions surface without a live server.

## Risks & open questions

- **HFS fork semantics** (`hfs_m_to_htime`, the 16-byte fork markers,
  MACR resource-fork frames) are the fiddliest bytes in `xfers.c`. Port
  with a dedicated Rust unit test built from captured wire bytes before
  wiring it live.
- **Preview path entanglement:** `get_thread` calls `preview_get` (the
  in-app preview streams through the same download path). The Rust
  worker needs a clean hook back to the C preview window, or preview
  stays a C-side variant of the get path. Decide in F2.
- **Cancellation + refcount** cross the FFI boundary (atomic flag +
  hxnet abort token + three ref owners). Keep the exact shape; don't
  redesign lifetime during the port.
- **`--network=host` / large-file (>4 GiB) size64 path** must keep
  working — `test_large_file` guards it.
- No `docs/RUST-ROADMAP.md` currently exists (referenced in CLAUDE.md but
  absent); this doc can seed the files section of a refreshed roadmap.

## Rough effort

- **F1:** ~1–2 days (mechanical; establishes the crate).
- **F2:** ~1–2 weeks (the real work; slice by transfer direction).
- **F3:** ~3–5 days.
- **F4:** optional, defer.

Recommended start: **F1** to stand up `hxfiles-model` + the FFI pattern,
then **F2 sliced** (single-file get first) as the high-value core.
