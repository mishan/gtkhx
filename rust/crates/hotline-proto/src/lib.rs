//! Typed Rust representation of the Hotline wire protocol.
//!
//! This crate is the Phase R2 extraction target from `docs/RUST-ROADMAP.md`:
//! a typed, well-tested representation of every Hotline 1.0/1.2/1.5/1.9
//! message, plus a parser and serializer. It replaces the hand-written
//! `HN16`/`HN32` byte-swap macros and the `dh_start`/`dh_getint` chunk-walk
//! macros in `src/protocol.h`, and absorbs the Mac Roman <-> UTF-8 text
//! conversion that lives in `src/text_util.c`.
//!
//! ## Layering
//!
//! - [`wire`] — endianness-correct read/write primitives over `&[u8]`
//!   ([`wire::Decoder`], [`wire::Encoder`]) plus the length-prefixed
//!   data-chunk framing ([`wire::ChunkIter`]). This is the macro-replacement
//!   layer; everything else builds on it.
//! - [`messages`] — opcode and field-tag enums keyed to the wire constants
//!   in `src/hotline.h`. `#[non_exhaustive]` because mhxd's ChangeLog adds
//!   opcodes occasionally and we don't want a new server constant to be a
//!   breaking change for downstream `match`es.
//! - [`text`] — Mac Roman <-> UTF-8, matching glibc's `iconv` "MACINTOSH"
//!   table byte-for-byte (the table the C code reaches through `g_convert`).
//! - [`sanitize`] — in-place CR→LF and control-byte folding (`strip_ansi`),
//!   the post-parse cleanup the chat / message handlers apply.
//! - [`parse`] — typed parsers for individual server messages. Phase R2
//!   grows this one opcode at a time; the proof-of-concept opcodes are
//!   `HTLS_HDR_USER_SELFINFO` and `HTLS_HDR_TASK`.
//! - [`build`] — outgoing-message builders for the SEND path. Each
//!   `hx_send_*` in C delegates to a `build_*_chunks` here, then hands
//!   the chunk array to `hlwrite_chunks()` for actual wire encoding.
//! - `ffi` — `#[no_mangle] extern "C"` entry points the C dispatcher calls.
//!   Not part of the Rust-facing API; only compiled into the staticlib.
//!
//! ## FFI discipline
//!
//! Same convention as the Phase R1 crypto crates: the C side hand-declares
//! the `extern` prototypes, so signature drift surfaces as an undefined
//! symbol at link time. No cbindgen for this crate — the FFI surface is
//! small and opaque-pointer-free.

#![allow(unsafe_op_in_unsafe_fn)]

pub mod build;
pub mod inline_media;
pub mod messages;
pub mod parse;
pub mod sanitize;
pub mod text;
pub mod voice;
pub mod wire;

mod ffi;

/// Size of the fixed Hotline transaction header (`struct hl_hdr`):
/// `type`(4) + `trans`(4) + `flag`(4) + `len`(4) + `len2`(4) + `hc`(2).
/// Mirrors `SIZEOF_HL_HDR` in `src/hotline.h`.
pub const HL_HDR_LEN: usize = 22;

/// Size of a data-chunk header (`struct hl_data_hdr`): `type`(2) + `len`(2).
/// Mirrors `SIZEOF_HL_DATA_HDR` in `src/hotline.h`.
pub const HL_DATA_HDR_LEN: usize = 4;
