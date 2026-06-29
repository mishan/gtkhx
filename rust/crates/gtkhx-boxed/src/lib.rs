//! Phase R4.2 — the GObject **boxed payload value-types** carried by the
//! `GtkhxSession` signals, re-hosted from C into Rust.
//!
//! R4.1 left these as C types (`G_DEFINE_BOXED_TYPE` in `proto_helpers.c`
//! / `tracker_event.c`) that `gtkhx-session` referenced by `GType` over
//! FFI. R4.2 moves the boxed *type itself* — its `GType` registration
//! and its `_copy` / `_free` value semantics — here, one type at a time.
//! `HxMsgEvent` is done (R4.2a); `HxChatEvent` and `HxTrackerServer`
//! follow in R4.2b/c.
//!
//! # The struct layout stays C-visible
//!
//! The C producers (`hx_*_new` in `proto_helpers.c` / `tracker_event.c`)
//! still `g_new0` and fill these structs, and C consumers still read
//! their fields directly. So each type here is a `#[repr(C)]` mirror of
//! the C struct, with its byte layout pinned on both sides —
//! `_Static_assert(sizeof(...) == N)` in C against the `const _: () =
//! assert!(...)` blocks here. Same cross-language ABI-pin discipline R2
//! used for `HxChunk` and `gtkhx_proto_history_entry`.
//!
//! Memory is glib's: copy allocates with `g_malloc0` + `g_strndup`, free
//! releases with `g_free`, exactly as the deleted C boxed copy/free did
//! — so a value allocated by a C `hx_*_new` and a value produced by a
//! Rust `_copy` are released by the same `g_free`-based path regardless
//! of which side created it.
//!
//! # Why a separate crate from `gtkhx-session`
//!
//! These types are self-contained (glib only — no undefined externs into
//! the rest of GtkHx). Keeping them out of `gtkhx-session` (which still
//! externs the not-yet-ported boxed `GType`s) means a C target that
//! pulls `hx_msg_event_copy`/`_free` — e.g. the `test_msg_event` proto
//! unit test — links against *only* this self-contained archive and
//! never drags `gtkhx-session`'s dangling C externs in via codegen-unit
//! merging.

use glib::ffi::{g_free, g_malloc0, g_strndup, GType};
use glib::gobject_ffi::{g_boxed_type_register_static, GBoxedCopyFunc, GBoxedFreeFunc};
use std::ffi::c_char;
use std::mem::{offset_of, size_of};
use std::os::raw::c_void;
use std::ptr;
use std::sync::OnceLock;

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
unsafe fn register_once(
    cache: &OnceLock<usize>,
    name: *const c_char,
    copy: GBoxedCopyFunc,
    free: GBoxedFreeFunc,
) -> GType {
    *cache.get_or_init(|| g_boxed_type_register_static(name, copy, free) as usize) as GType
}

// ======================================================================
// HxMsgEvent — private-message value object (src/proto_helpers.h).
// ======================================================================

/// `#[repr(C)]` mirror of `struct _HxMsgEvent`. Field order/types must
/// match `src/proto_helpers.h` byte-for-byte; pinned below and by the
/// `_Static_assert` in `proto_helpers.c`.
#[repr(C)]
pub struct HxMsgEvent {
    pub uid: u16,
    pub name: *mut c_char,
    pub name_len: usize,
    pub body: *mut c_char,
    pub body_len: usize,
    pub is_self: i32,      // gboolean
    pub is_broadcast: i32, // gboolean
}

const _: () = {
    assert!(size_of::<HxMsgEvent>() == 48);
    assert!(offset_of!(HxMsgEvent, uid) == 0);
    assert!(offset_of!(HxMsgEvent, name) == 8);
    assert!(offset_of!(HxMsgEvent, name_len) == 16);
    assert!(offset_of!(HxMsgEvent, body) == 24);
    assert!(offset_of!(HxMsgEvent, body_len) == 32);
    assert!(offset_of!(HxMsgEvent, is_self) == 40);
    assert!(offset_of!(HxMsgEvent, is_broadcast) == 44);
};

/// Boxed copy func. Deep-copies the two owned strings; mirrors the
/// deleted C `hx_msg_event_copy` (`*c = *e` then `g_strndup` name/body).
///
/// # Safety
/// `e` is NULL or a valid `HxMsgEvent*` with `g_malloc`-owned `name`/`body`.
#[no_mangle]
pub unsafe extern "C" fn hx_msg_event_copy(e: *mut HxMsgEvent) -> *mut HxMsgEvent {
    if e.is_null() {
        return ptr::null_mut();
    }
    let c = g_malloc0(size_of::<HxMsgEvent>()) as *mut HxMsgEvent;
    (*c).uid = (*e).uid;
    (*c).name_len = (*e).name_len;
    (*c).body_len = (*e).body_len;
    (*c).is_self = (*e).is_self;
    (*c).is_broadcast = (*e).is_broadcast;
    // g_strndup(NULL, n) returns NULL — matches the C path for the
    // (not-expected) NULL-string case.
    (*c).name = g_strndup((*e).name, (*e).name_len);
    (*c).body = g_strndup((*e).body, (*e).body_len);
    c
}

/// Boxed free func. Mirrors the deleted C `hx_msg_event_free`.
///
/// # Safety
/// `e` is NULL or a valid `HxMsgEvent*` whose `name`/`body`/self are
/// `g_malloc`-owned.
#[no_mangle]
pub unsafe extern "C" fn hx_msg_event_free(e: *mut HxMsgEvent) {
    if e.is_null() {
        return;
    }
    g_free((*e).name as *mut c_void);
    g_free((*e).body as *mut c_void);
    g_free(e as *mut c_void);
}

/// `HX_TYPE_MSG_EVENT` accessor — the C ABI the old
/// `G_DEFINE_BOXED_TYPE (HxMsgEvent, hx_msg_event, …)` exported.
#[no_mangle]
pub extern "C" fn hx_msg_event_get_type() -> GType {
    static TYPE: OnceLock<usize> = OnceLock::new();
    unsafe {
        register_once(
            &TYPE,
            c"HxMsgEvent".as_ptr(),
            Some(std::mem::transmute::<
                unsafe extern "C" fn(*mut HxMsgEvent) -> *mut HxMsgEvent,
                unsafe extern "C" fn(*mut c_void) -> *mut c_void,
            >(hx_msg_event_copy)),
            Some(std::mem::transmute::<
                unsafe extern "C" fn(*mut HxMsgEvent),
                unsafe extern "C" fn(*mut c_void),
            >(hx_msg_event_free)),
        )
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Build a heap `HxMsgEvent` the way a C producer would (glib-owned
    /// strings), so copy/free exercise the real allocation path.
    unsafe fn make(uid: u16, name: &str, body: &str, is_self: bool) -> *mut HxMsgEvent {
        let e = g_malloc0(size_of::<HxMsgEvent>()) as *mut HxMsgEvent;
        (*e).uid = uid;
        (*e).is_broadcast = (uid == 0) as i32;
        (*e).is_self = is_self as i32;
        (*e).name = g_strndup(name.as_ptr() as *const c_char, name.len());
        (*e).name_len = name.len();
        (*e).body = g_strndup(body.as_ptr() as *const c_char, body.len());
        (*e).body_len = body.len();
        e
    }

    unsafe fn cstr(p: *const c_char, len: usize) -> String {
        let slice = std::slice::from_raw_parts(p as *const u8, len);
        String::from_utf8_lossy(slice).into_owned()
    }

    #[test]
    fn get_type_is_a_registered_boxed_type() {
        let t = hx_msg_event_get_type();
        assert_ne!(t, 0);
        assert_eq!(t, hx_msg_event_get_type(), "registered exactly once");
        let ty: glib::Type = unsafe { glib::translate::from_glib(t) };
        assert!(ty.is_a(glib::Type::BOXED));
    }

    #[test]
    fn copy_is_a_deep_copy() {
        unsafe {
            let a = make(42, "alice", "hello world", true);
            let b = hx_msg_event_copy(a);
            assert!(!b.is_null());
            assert_ne!(a, b);
            assert_ne!((*a).name, (*b).name);
            assert_ne!((*a).body, (*b).body);
            assert_eq!((*b).uid, 42);
            assert_eq!((*b).is_self, 1);
            assert_eq!((*b).is_broadcast, 0);
            assert_eq!(cstr((*b).name, (*b).name_len), "alice");
            assert_eq!(cstr((*b).body, (*b).body_len), "hello world");
            // Freeing the original must not disturb the copy.
            hx_msg_event_free(a);
            assert_eq!(cstr((*b).name, (*b).name_len), "alice");
            hx_msg_event_free(b);
        }
    }

    #[test]
    fn copy_and_free_are_null_safe() {
        unsafe {
            assert!(hx_msg_event_copy(ptr::null_mut()).is_null());
            hx_msg_event_free(ptr::null_mut()); // no crash
        }
    }

    #[test]
    fn g_boxed_copy_roundtrips_through_the_registered_funcs() {
        // Exercise the type the way GLib's signal-emit boxed marshaling
        // does: g_boxed_copy / g_boxed_free via the registered GType.
        unsafe {
            let t = hx_msg_event_get_type();
            let a = make(7, "bob", "hi", false);
            let b = glib::gobject_ffi::g_boxed_copy(t, a as *mut c_void) as *mut HxMsgEvent;
            assert_ne!(a, b);
            assert_eq!(cstr((*b).name, (*b).name_len), "bob");
            glib::gobject_ffi::g_boxed_free(t, b as *mut c_void);
            hx_msg_event_free(a);
        }
    }
}
