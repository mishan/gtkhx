//! Bounded image decoder for GtkHx's inline-media extension
//! (`docs/inline-media-plan.md` Phase B/E, `docs/glycin-migration-plan.md`).
//!
//! G.1 shipped the magic-byte sniff layer; G.2 (this commit)
//! adds the glycin-driven decode pipeline. The C side talks to us
//! through the FFI shims in [`ffi`]:
//!
//! - [`ffi::hx_image_decode_sniff`] — magic-byte sniff (G.1).
//! - [`ffi::hx_image_decode_format_is_allowed`] — JPEG/PNG/GIF
//!   gate (G.1).
//! - [`ffi::hx_image_decode_format_to_mime`] — canonical MIME
//!   string (G.1).
//! - [`ffi::inline_media_decode_async`] — async decode entry,
//!   replaces the C-side `inline_media_decode` (G.2).
//! - [`ffi::inline_media_decode_cancel`] — cancel + release the
//!   token returned by `_async`.
//! - [`ffi::inline_media_decoded_free`] — release the result
//!   handed to the C callback.
//!
//! G.3 adds an animation-frame iterator on top of the same
//! result struct (`frames` field reserved in G.2).
//!
//! # Defence-in-depth layout
//!
//! `sniff` is the first gate. Per the inline-media spec, only
//! JPEG / PNG / GIF are allowed; SVG / WebP / AVIF / HEIC / TIFF
//! / ICO / BMP are *explicitly recognised* so the rejection log
//! line is honest about WHY the bytes were rejected. Even if a
//! future glycin loader adds support for a blocked format, the
//! sniff layer keeps that out of the decode pipeline.
//!
//! `sniff` is intentionally pure — no `gio`, `gdk`, or `glib`
//! calls, no allocations, bounded scan window (≤32 bytes
//! regardless of input length). Unit-testable in any CI
//! container.
//!
//! `decode` runs glycin's sandboxed subprocess loader; sandbox
//! escape is glycin's design contract, not ours.

#![allow(unsafe_op_in_unsafe_fn)]

pub(crate) mod caps;
/// gtk-rs + glycin version aliases. `#[doc(hidden)] pub` so the
/// out-of-library integration test crate can reuse the same family.
#[doc(hidden)]
pub mod compat;
pub mod decode;
pub mod ffi;
pub(crate) mod ffi_result;
pub mod sniff;
pub(crate) mod telemetry;

pub use sniff::{format_is_allowed, format_to_mime, sniff, Format};

// Closure-based, all-Rust decode entry for in-tree gtk4-rs callers (the banner),
// so they get a `gdk::Texture` directly instead of driving the C-callback +
// `HxInlineMediaDecoded` FFI shape.
pub use decode::{decode_first_frame_async, ImageDecodeHandle, ImageDecodeOutcome};
