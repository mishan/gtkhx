# `hxhfs` — HFS metadata sidecar crate

Standalone Rust port of `src/hfs.c`, the HFS-metadata sidecar reader/writer, to
seed the (unmerged) `hxfiles-xfer` transfer crate. **Standalone for now:** the
crate builds + unit-tests in the workspace but is **not linked into `src/gtkhx`
yet** — `hfs.c` is untouched, so there is no binary behavior change.

## Why

Classic Mac files carry a **resource fork** and **Finder info** (4-byte type +
4-byte creator, creation / modification dates, and a comment) that a plain Unix
filesystem can't hold. When a Hotline transfer moves such a file to/from disk,
that metadata is preserved in a *companion sidecar file* next to the data file,
in one of three historical layouts:

- **CAP** (`aufs`) — a `.rsrc` file holds the raw resource fork; a `.fndrinfo`
  file holds a fixed 300-byte record (`struct hfs_cap_info`).
- **AppleDouble** (v2) / **Netatalk** (v1 magic) — one `.fndrinfo` file holds a
  header (magic + version + a table of typed descriptors) whose entries point at
  the Finder info, dates, comment, and resource fork all within that one file.

`hfs.c`'s callers are `xfers.c` (the transfer worker: reads type/creator +
Finder info + resource fork on upload, writes them on download) and `rcv.c` (the
get-info size math). Both belong to the transfer path `hxfiles-xfer` will own, so
the sidecar logic wants to be a Rust crate that crate can depend on.

## Layout

Idiomatic native API + a thin FFI shim (the chosen shape):

- `src/hfs.rs` — the **native API** `hxfiles-xfer` will call. Explicit
  [`Config`] (`Fork` / perms / default comment / `dir_char`), `&Path` inputs,
  `io::Result` / owned `HfsInfo` outputs. Covers `finderinfo_path` /
  `resource_path`, `resource_open` / `resource_len`, `type_creator`,
  `hfsinfo_read` / `hfsinfo_write`, and `comment_len` / `comment_write`, across
  all three fork layouts. `HfsInfo` keeps the timestamps as raw 4-byte "header"
  (2000-epoch, big-endian) values so the on-disk bytes round-trip exactly.
- `src/suffix.rs` — the extension → type/creator fallback table
  (`suffix_type_creator`), byte-exact and case-sensitive as the C `strcmp` chain.
- `src/ffi.rs` — the **`#[no_mangle]` C-ABI shim** preserving the exact `hfs.h`
  surface: the symbol names, a `#[repr(C)]` `struct hfsinfo` mirror (ABI pinned
  with `const _: () = assert!(size_of == 224)`), and the process-global config
  (`hfs_set_config`). This is the drop-in seam for a later leaf-up replacement of
  `hfs.c` — when that lands, delete `hfs.c` and link this crate's staticlib.

## Compatibility

Wire compatibility is a hard requirement: real Mac-aware tools (`netatalk`, the
original Hotline clients) read and write these same files, so the on-disk bytes
must match `hfs.c` byte-for-byte. Pinned by the round-trip + golden-byte tests
(CAP record offsets, AppleDouble header magic/version/entry-count, resource-fork
read/write, comment clamping, suffix table, sidecar-path splitting).

Faithfulness notes / intentional divergences from the C:

- `hfs_set_config` has **no caller** in the tree today (the fork is always the
  CAP default); all three layouts are still modelled for `hxfiles-xfer`.
- `comment_write` only supports CAP (as in `hfs.c`; AppleDouble is a no-op).
- `resource_open` takes a caller-provided `OpenOptions` (the native API) /
  translates the C `mode` + `perm` `open(2)` args into one (the FFI shim, using
  `libc`'s platform-correct `O_*` constants), so the full open policy — read /
  write / create / truncate / append / mode — passes through exactly as `hfs.c`
  did. It never implicitly truncates, so a resumed transfer can open + seek
  before writing. The native `resource_open` maps a missing *target file* to
  `Ok(None)` (the "no resource fork, skip" case) but propagates a `NotFound`
  whose *parent directory* is what's missing — a wrong path / failed create
  isn't silently swallowed. The **FFI** shim normalizes every "no resource fork"
  outcome to **-1**, deliberately dropping `hfs.c`'s AppleDouble "return `0`
  (stdin fd) on open failure" quirk — the `xfers.c` `< 0` checks already handle
  it, but that's a documented behavior change to reconcile at wire-up time.
- `hfsinfo_write` (CAP) opens with create + no truncate and overwrites the first
  300 bytes from offset 0, leaving any trailing bytes intact — byte-faithful to
  `hfs.c`'s `O_RDWR|O_CREAT`.
- The FFI shim uses the platform-exact `MAXPATHLEN` (`min(PATH_MAX, 4095)`, via
  `libc`) when writing into a C caller's `char buf[MAXPATHLEN]`, so it can't
  overrun on a platform where `PATH_MAX < 4095`. The native length cap is the
  4095 ceiling (it returns a heap `PathBuf`, not a fixed buffer).
- The FFI `finderinfo_path` / `resource_path` honor the legacy `statbuf`
  out-param: when non-NULL they `stat` the sidecar into it and return the
  failure `errno` (the existence check the C callers rely on).
- The FFI config accessors recover from a poisoned mutex (`into_inner`) rather
  than `unwrap`-panicking — an unwind across `extern "C"` would be UB.

## Follow-ups (not in this crate yet)

- **Wire it up** when `hxfiles-xfer` lands: depend on the native API from Rust,
  and/or do the leaf-up C replacement (delete `hfs.c`, link the FFI shim; port
  `dir_char` handling from `files.c` into the config).
- **`macres.c` + `cicn.c` → Rust** — the Mac resource-fork parser + color-icon
  renderer (the user-icon picker) are a separate future port, not transfer-side.
