//! `HxMsgEvent` — private-message value object (`src/proto_helpers.h`).
//! R4.2a. See the crate docs for the cross-language layout-pin contract.

use crate::boxed::register_once;
use glib::ffi::{g_free, g_malloc0, g_strndup, GType};
use std::ffi::c_char;
use std::mem::{offset_of, size_of};
use std::os::raw::c_void;
use std::ptr;
use std::sync::OnceLock;

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

/// `GBoxedCopyFunc`-shaped shim: matches `unsafe extern "C" fn(gpointer)
/// -> gpointer` exactly and delegates to the typed [`hx_msg_event_copy`],
/// so the boxed-type registration needs no `transmute` (which would
/// silently become UB if the typed signature ever drifted).
///
/// # Safety
/// `p` is NULL or a valid `HxMsgEvent*`.
unsafe extern "C" fn boxed_copy(p: *mut c_void) -> *mut c_void {
    hx_msg_event_copy(p as *mut HxMsgEvent) as *mut c_void
}

/// `GBoxedFreeFunc`-shaped shim — see [`boxed_copy`].
///
/// # Safety
/// `p` is NULL or a valid `HxMsgEvent*`.
unsafe extern "C" fn boxed_free(p: *mut c_void) {
    hx_msg_event_free(p as *mut HxMsgEvent);
}

/// `HX_TYPE_MSG_EVENT` accessor — the C ABI the old
/// `G_DEFINE_BOXED_TYPE (HxMsgEvent, hx_msg_event, …)` exported.
#[no_mangle]
pub extern "C" fn hx_msg_event_get_type() -> GType {
    static TYPE: OnceLock<usize> = OnceLock::new();
    unsafe { register_once(&TYPE, c"HxMsgEvent".as_ptr(), Some(boxed_copy), Some(boxed_free)) }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Build a heap `HxMsgEvent` the way a C producer would (glib-owned
    /// strings), so copy/free exercise the real allocation path.
    unsafe fn make(uid: u16, name: &str, body: &str, is_self: bool) -> *mut HxMsgEvent {
        let e = g_malloc0(size_of::<HxMsgEvent>()) as *mut HxMsgEvent;
        assert!(!e.is_null(), "g_malloc0 returned NULL");
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
            assert!(!a.is_null());
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
        unsafe {
            let t = hx_msg_event_get_type();
            let a = make(7, "bob", "hi", false);
            assert!(!a.is_null());
            let b = glib::gobject_ffi::g_boxed_copy(t, a as *mut c_void) as *mut HxMsgEvent;
            assert!(!b.is_null());
            assert_ne!(a, b);
            assert_eq!(cstr((*b).name, (*b).name_len), "bob");
            glib::gobject_ffi::g_boxed_free(t, b as *mut c_void);
            hx_msg_event_free(a);
        }
    }
}
