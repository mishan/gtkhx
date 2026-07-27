//! Phase R4.2 — the GObject **boxed payload value-types** carried by the
//! `GtkhxSession` signals, re-hosted from C into Rust.
//!
//! R4.1 left these as C types (`G_DEFINE_BOXED_TYPE` in `proto_helpers.c`
//! / `tracker_event.c`) that `gtkhx-session` referenced by `GType` over
//! FFI. R4.2 moves the boxed *type itself* — its `GType` registration
//! and its `_copy` / `_free` value semantics — here, one type at a time:
//!
//!   - [`msg`]     — `HxMsgEvent`     (R4.2a ✅)
//!   - [`chat`]    — `HxChatEvent` + `HxChatMedia` (R4.2c ✅)
//!   - [`tracker`] — `HxTrackerServer` + `HxTrackerV3Meta` (R4.2b ✅)
//!
//! # The struct layout stays C-visible
//!
//! The C producers (`hx_*_new`, `hx_chat_event_attach_media`) still
//! `g_new0` and fill these structs, and C consumers (and the media
//! placeholder formatters) still read their fields directly. So each
//! type is a `#[repr(C)]` mirror of the C struct, byte layout pinned on
//! both sides — `_Static_assert(sizeof(...) == N)` in C against the
//! `const _: () = assert!(offset_of!(...))` blocks here. Same
//! cross-language ABI-pin discipline R2 used for `HxChunk`.
//!
//! Memory is glib's: copy allocates with `g_malloc0` / `g_malloc` /
//! `g_strndup`, free releases with `g_free`, exactly as the deleted C
//! boxed copy/free did — so a value allocated by a C `hx_*_new` and a
//! value produced by a Rust `_copy` are released by the same
//! `g_free`-based path regardless of which side created it.
//!
//! # Why a separate crate from `gtkhx-session`
//!
//! These types are self-contained (glib only — no undefined externs into
//! the rest of GtkHx). Keeping them out of `gtkhx-session` (which still
//! externs the not-yet-ported boxed `GType`s) means a C target that
//! pulls a `_copy`/`_free` symbol — e.g. the `test_msg_event` /
//! `test_chat_event` proto unit tests — links against *only* this
//! self-contained archive and never drags `gtkhx-session`'s dangling C
//! externs in via codegen-unit merging.

use glib::ffi::GType;
use glib::gobject_ffi::{g_boxed_type_register_static, GBoxedCopyFunc, GBoxedFreeFunc};
use std::ffi::c_char;
use std::sync::OnceLock;

pub mod chat;
pub mod media_table;
pub mod msg;
pub mod tracker;

/// Register a boxed `GType` exactly once for `name`, with the given
/// copy/free funcs. The `GType` is cached as a `usize` — a `GType` is a
/// genuine integer handle, not a pointer, so there is no provenance
/// concern in the round-trip (unlike a real object pointer).
/// `g_boxed_type_register_static` must run only once per name (a second
/// call would re-register and `g_warning`), which the `OnceLock`
/// guarantees.
///
/// # Safety
/// `copy`/`free` must be valid boxed copy/free funcs for `name`'s type,
/// and `name` a static NUL-terminated C string.
pub(crate) unsafe fn register_once(
    cache: &OnceLock<usize>,
    name: *const c_char,
    copy: GBoxedCopyFunc,
    free: GBoxedFreeFunc,
) -> GType {
    *cache.get_or_init(|| g_boxed_type_register_static(name, copy, free) as usize) as GType
}
