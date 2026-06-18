//! Bounded image decoder for GtkHx's inline-media extension
//! (`docs/inline-media-plan.md` Phase B/E, `docs/glycin-migration-plan.md`).
//!
//! G.1 (this commit) ships the magic-byte sniff layer and the FFI
//! shape. Sniff was previously in `src/inline_media_decode.c` —
//! pure-byte logic, easy port. Moving it here lets the same code be
//! consumed both from the C-side bridge (via the `#[no_mangle] extern
//! "C"` functions in `ffi`) and from later Rust callers (e.g. the
//! decoder pipeline that lands in G.2 once the glycin dep arrives).
//!
//! G.2+ will add:
//! - `decoder` module wrapping `glycin::Loader` for the actual decode
//! - `ffi` exports for `hx_image_decode_async` + cancel token
//! - Animation frame iteration (G.3)
//!
//! # Defence-in-depth layout
//!
//! `sniff` is the first gate. Per the inline-media spec, only JPEG /
//! PNG / GIF are allowed; SVG / WebP / AVIF / HEIC / TIFF / ICO / BMP
//! are *explicitly recognised* so the rejection log line is honest
//! about WHY the bytes were rejected (rather than a generic "unknown
//! format"). Even if a future glycin loader adds support for a
//! blocked format, the sniff layer keeps that out of the decode
//! pipeline.
//!
//! `sniff` is intentionally pure — no `gio`, `gdk`, or `glib` calls,
//! no allocations, bounded scan window (≤32 bytes regardless of input
//! length). Unit-testable in any CI container.

#![allow(unsafe_op_in_unsafe_fn)]

pub mod ffi;
pub mod sniff;

pub use sniff::{format_is_allowed, format_to_mime, sniff, Format};
