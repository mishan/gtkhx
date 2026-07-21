# Files subsystem — C→Rust migration scope

> Scoping document. Maps the components, what's already in Rust, what's
> worth moving, and a phased plan with risk/effort. **F1 and F2 are done**
> — see the Status section below.

## Status

- **F1 — pure helpers: DONE.** The file-type → icon-id map and the
  FourCC → human-label table live in the `hxfiles-model` crate
  (`icon_id_for`, `kind_label_for`), called from `files.c` via the
  `gtkhx_files_*` FFI.
- **F2 — the transfer workers: DONE**, though it landed differently from
  the original "full tokio port" sketch below (see the F2 section for the
  as-built architecture). The FFO/FILP frame codec + HFS fork byte math
  moved to the dependency-free `hxfiles-xfer` crate (`ffo` module,
  unit-tested headless). The four worker state machines were **extracted
  from `xfers.c` into linkable C units** — `file_recv_one` /
  `folder_recv_all` in `src/xfers_recv.c`, `file_send_one` /
  `folder_send_all` in `src/xfers_send.c` — each driven by a
  one-function-pointer progress seam so they can be tested without the
  GTK worker shell. `xfers.c` keeps only the worker shell (the `xfers[]`
  list, refcount lifecycle, dispatch, and the thin connect/sound
  wrappers). New worker-level round-trip tests drive the real code
  against mhxd: `test_file_get`, `test_file_put`, `test_folder_roundtrip`.
  - **Bug found + fixed along the way:** `folder_send_all` (the folder
    upload path) declared a per-file size that excluded the 16-byte MACR
    marker `file_send_one` always writes, so multi-file folder uploads
    desynced the server and failed. Never caught because the solo-file
    path has no following file to corrupt and folder upload had never
    been driven end-to-end. Fixed in the F2 folder commit.
- **F3 (remote provider model) and F4 (provider trait): not started.**

### F2 follow-ups (deferred)

- **Nested-subdir folder transfer — RESOLVED (a mhxd limitation, not a
  client bug).** A full nested round-trip via GETFOLDER can't retrieve
  subdir files from mhxd: its `folder_send` (server download) is
  non-recursive — `folder_getpaths` reads a single directory level and
  `folder_send` hard-codes `pathcount = 1`, so it emits a marker for a
  subdir but never descends. `folder_send_all` (upload) *does* correctly
  send the recursive tree, and mhxd's `folder_recv` stores it;
  `test_folder_roundtrip`'s `nested_upload` case proves this by uploading
  `alpha.txt` + `nested/beta.txt` and fetching each back with a direct
  FILE_GET (including the subdir file). `folder_recv_all` also now
  `mkdir -p`s each file's parent before writing, so it reconstructs a
  nested tree correctly from any recursion-capable server (independent of
  mhxd's download limit).
- **`hxhfs` wiring.** The HFS sidecar (`type_creator`, `hfsinfo_read/
  write`, `resource_open`, `comment_len`) now has a standalone Rust port
  in the `hxhfs` crate, but `xfers_send.c` / `xfers_recv.c` still call the
  C `hfs.c`. Switching them to `hxhfs` (and retiring `hfs.c`) is a natural
  next step, coordinated with the `hxhfs` work.
- **`htxf_io.c` / `htxf_subchannel.c`** remain thin C shims rather than
  being folded into a Rust worker — the as-built F2 keeps the I/O loops
  in C (see the F2 section).

> Scoping notes below predate the F1/F2 work; kept for the component
> inventory + F3/F4 planning.

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

### Phase F1 — pure helpers (warm-up, low risk) — **DONE**
- Moved to the `hxfiles-model` crate: `icon_of_ftype_and_name` →
  `icon_id_for`, `kind_of_ftype` → `kind_label_for`. `files.c` keeps the
  C signatures and calls the `gtkhx_files_*` FFI; the `_()` translation +
  "Unknown"/unknown-FourCC fallbacks stay in C.
- (The `hx_htxf_subchannel_pack_preamble` / `filelist_walker` / size
  formatters listed in the original sketch were left in C — low value,
  and the preamble packer is already a thin leaf over Rust.)
- **Test:** `cargo test -p hxfiles-model` (icon + label tables).

### Phase F2 — the transfer workers — **DONE (as-built differs)**

The original sketch was a "full tokio port" that also folded in
`htxf_io.c` / `htxf_subchannel.c`. As built, F2 drew the boundary
differently, and the reasoning is worth recording:

- **What moved to Rust:** only the genuinely protocol-shaped, error-prone
  *byte math* — the FFO fork-header decode/encode (legacy + large-file
  high32/low32 split), the FILP info-block parse, the mac↔header (1904↔
  2000) wire epoch conversion, and the info-block length. All in the
  dependency-free `hxfiles-xfer` crate's `ffo` module, unit-tested
  headless (no socket, no server), pinned against the same wire-shape
  oracle `tests/proto/test_large_file.c` uses. Exposed to C via the
  `gtkhx_ffo_*` FFI.
- **What stayed in C but was extracted for testability:** the four worker
  state machines. `file_recv_one` + `folder_recv_all` → `src/xfers_recv.c`;
  `file_send_one` + `folder_send_all` (+ the DFS tree walk) →
  `src/xfers_send.c`. The one coupling back to the GTK worker shell —
  `post_file_update` — became a `xfer_progress_fn` function-pointer
  parameter, so each machine links + runs without GTK. `xfers.c` keeps
  the shell: the `xfers[]` list, refcount lifecycle, worker/completion
  dispatch, and the thin `htxf_connect` / `play_sound` / completion
  wrappers.
- **Why not a full tokio port:** the I/O loops are thin plumbing over
  `hxnet` (already Rust) and the HFS sidecar / preview window are
  C/GTK-shaped. Re-expressing them as a Rust worker would mean a large
  FFI callback vtable or a fragile `htxf_conn` struct mirror, for little
  gain over "protocol math in Rust, I/O + platform glue in C" — which
  matches the project's MVC boundary. `htxf_io.c` / `htxf_subchannel.c`
  stay as the thin shims they already are.
- **Tests (worker-level, against the live mhxd rig):**
  `test_file_get` and `test_file_put` drive the real `file_recv_one` /
  `file_send_one` and assert byte-exact on disk; `test_folder_roundtrip`
  uploads a tree via `folder_send_all` and downloads it back through
  `folder_recv_all`. Each was verified to *fail* under a deliberate
  fault injection. Plus the pre-existing net (`test_large_file`,
  `test_folder_xfer`, `test_xfer_queue`, `test_htxf_cancel`,
  `test_htxf_hdr`, `test_real_htxf_connect`).
- **Bug fixed:** the `folder_send_all` MACR-size desync (see Status).

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

Added by F1/F2:

- **unit (Rust):** `cargo test -p hxfiles-model` (icon + label tables),
  `cargo test -p hxfiles-xfer` (the FFO frame codec + HFS fork math,
  headless).
- **integration (worker-level, mhxd):** `test_file_get` and
  `test_file_put` now drive the real `file_recv_one` / `file_send_one`
  and assert byte-exact on disk (rewritten from harness-only smoke
  tests); `test_folder_roundtrip` drives `folder_send_all` +
  `folder_recv_all` end to end.

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

- **F1:** DONE.
- **F2:** DONE.
- **F3:** ~3–5 days (not started).
- **F4:** optional, defer.

Next up: **F3** — extract the remote listing model + reply handling from
`files_remote_provider.c` into `hxfiles-model`, keeping the rcv.c signal
routing + timeout watchdog in C. Smaller F2 follow-ups (nested-subdir
folder round-trip, `hxhfs` wiring) are listed in the Status section.
