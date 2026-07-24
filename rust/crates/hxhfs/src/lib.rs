//! `hxhfs` — HFS metadata sidecar reader/writer, ported from `src/hfs.c`.
//!
//! Classic Mac files carry two extra pieces of metadata a plain Unix filesystem
//! can't hold: a **resource fork** and **Finder info** (the 4-byte type +
//! 4-byte creator codes, creation / modification dates, and a comment). When a
//! Hotline transfer moves such a file to/from disk, that metadata is preserved
//! in a *companion sidecar file* next to the data file, in one of three
//! historical layouts:
//!
//! - **CAP** (`aufs`) — a `.rsrc` file holds the raw resource fork and a
//!   `.fndrinfo` file holds a fixed 300-byte [`format::cap`] record.
//! - **AppleDouble** / **Netatalk** — a single `.fndrinfo` file holds an
//!   AppleDouble header (a magic + version + a table of typed descriptors) whose
//!   entries point at the Finder info, dates, comment, and resource fork all
//!   within that one file.
//!
//! This crate is the byte-faithful reader/writer for those sidecars. The
//! [`hfs`] module is the idiomatic native API (explicit [`Config`], `&Path`
//! inputs, `io::Result` / owned [`HfsInfo`] outputs) that the future
//! `hxfiles-xfer` crate will call. The [`ffi`] module is a thin `#[no_mangle]`
//! shim preserving the exact `hfs.h` C ABI (including the global-config
//! `hfs_set_config` and the `struct hfsinfo` layout) for the eventual leaf-up
//! replacement of `hfs.c`.
//!
//! Wire compatibility is a hard requirement: real Mac-aware tools (`netatalk`,
//! the original Hotline clients) read and write these same files, so the
//! on-disk bytes must match `hfs.c` exactly. The round-trip + golden-byte tests
//! pin that.

// The `ffi` shim preserves hfs.c's C ABI. On Windows it bridges the resource-fork
// `File` to a CRT file descriptor via `_open_osfhandle` (the C callers read /
// write / lseek / close it), which works because MSYS2 UCRT64 links the same
// UCRT into Rust and the C app, so the descriptor table is shared. The native
// `hfs` module below is portable on its own.
pub mod ffi;
pub mod hfs;
mod suffix;

pub use hfs::{Config, Fork, HfsInfo, MAX_COMMENT};
