//! The GObject **boxed payload value-types** carried by the `GtkhxSession`
//! signals.
//!
//! These began as C types (`G_DEFINE_BOXED_TYPE` in `proto_helpers.c` /
//! `tracker_event.c`) that the session referenced by `GType` over FFI. The
//! boxed *type itself* — its `GType` registration and its `_copy` / `_free`
//! value semantics — now lives here:
//!
//!   - [`msg`]     — `HxMsgEvent`
//!   - [`chat`]    — `HxChatEvent` + `HxChatMedia`
//!   - [`tracker`] — `HxTrackerServer` + `HxTrackerV3Meta`
//!   - [`history`] — `HxHistoryEntry` (chat-history; a plain value struct
//!     carried in a `GPtrArray`, **not** a registered `GType`)
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
//! # These types must stay extern-free
//!
//! They are self-contained — glib only, no undefined externs into the
//! rest of GtkHx — and that is load-bearing, not incidental. A C target
//! that pulls a `_copy`/`_free` symbol (the `test_msg_event` /
//! `test_chat_event` proto unit tests) links the `gtkhx-core` archive
//! directly rather than the `gtkhx-ffi` façade, so an extern added here
//! becomes an unresolved reference in those tests. See the crate-level
//! note in `lib.rs`.

use glib::ffi::GType;
use glib::gobject_ffi::{g_boxed_type_register_static, GBoxedCopyFunc, GBoxedFreeFunc};
use std::ffi::c_char;
use std::sync::OnceLock;

pub mod chat;
pub mod history;
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
