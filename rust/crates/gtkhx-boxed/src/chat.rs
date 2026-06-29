//! `HxChatEvent` + nested `HxChatMedia` — chat-line value object
//! (`src/proto_helpers.h`). R4.2c.
//!
//! Only the boxed type moves: `hx_chat_event_new` (the wire-bytes parse),
//! `hx_chat_event_attach_media`, and the media placeholder formatters
//! stay in C and keep reading/writing these `#[repr(C)]` structs. The
//! nested `HxChatMedia` copy/free are private here (the C side's were
//! `static`); C keeps its own `hx_chat_media_free` because
//! `hx_chat_event_attach_media` still calls it.

use crate::register_once;
use glib::ffi::{g_free, g_malloc, g_malloc0, g_strndup, GType};
use std::ffi::c_char;
use std::mem::{offset_of, size_of};
use std::os::raw::c_void;
use std::ptr;
use std::sync::OnceLock;

/// `#[repr(C)]` mirror of `HxChatMedia` (optional inline-media metadata
/// attached to a chat event). Owned by the parent `HxChatEvent`.
#[repr(C)]
pub struct HxChatMedia {
    pub id: *mut u8, // opaque handle bytes (not NUL-terminated)
    pub id_len: usize,
    pub mime: *mut c_char, // canonical MIME (NUL-terminated)
    pub mime_len: usize,
    pub width: u32,
    pub height: u32,
    pub bytes: u32,
    pub width_present: i32,  // gboolean
    pub height_present: i32, // gboolean
    pub bytes_present: i32,  // gboolean
}

const _: () = {
    assert!(size_of::<HxChatMedia>() == 56);
    assert!(offset_of!(HxChatMedia, id) == 0);
    assert!(offset_of!(HxChatMedia, id_len) == 8);
    assert!(offset_of!(HxChatMedia, mime) == 16);
    assert!(offset_of!(HxChatMedia, mime_len) == 24);
    assert!(offset_of!(HxChatMedia, width) == 32);
    assert!(offset_of!(HxChatMedia, height) == 36);
    assert!(offset_of!(HxChatMedia, bytes) == 40);
    assert!(offset_of!(HxChatMedia, width_present) == 44);
    assert!(offset_of!(HxChatMedia, height_present) == 48);
    assert!(offset_of!(HxChatMedia, bytes_present) == 52);
};

/// `#[repr(C)]` mirror of `struct _HxChatEvent`.
#[repr(C)]
pub struct HxChatEvent {
    pub cid: u32,
    pub line: *mut c_char, // UTF-8, NUL-terminated, owned
    pub line_len: usize,
    pub sender_off: usize,
    pub sender_len: usize,
    pub body_off: usize,
    pub body_len: usize,
    pub is_info: i32, // gboolean
    pub is_self: i32, // gboolean
    pub media: *mut HxChatMedia,
}

const _: () = {
    assert!(size_of::<HxChatEvent>() == 72);
    assert!(offset_of!(HxChatEvent, cid) == 0);
    assert!(offset_of!(HxChatEvent, line) == 8);
    assert!(offset_of!(HxChatEvent, line_len) == 16);
    assert!(offset_of!(HxChatEvent, sender_off) == 24);
    assert!(offset_of!(HxChatEvent, sender_len) == 32);
    assert!(offset_of!(HxChatEvent, body_off) == 40);
    assert!(offset_of!(HxChatEvent, body_len) == 48);
    assert!(offset_of!(HxChatEvent, is_info) == 56);
    assert!(offset_of!(HxChatEvent, is_self) == 60);
    assert!(offset_of!(HxChatEvent, media) == 64);
};

/// Deep-copy an `HxChatMedia` (private; mirrors the deleted C static
/// `hx_chat_media_copy`). `id` is raw bytes (`g_malloc` + copy), `mime`
/// is NUL-terminated (`g_strndup`).
///
/// # Safety
/// `m` is NULL or a valid `HxChatMedia*` with glib-owned `id`/`mime`.
unsafe fn media_copy(m: *const HxChatMedia) -> *mut HxChatMedia {
    if m.is_null() {
        return ptr::null_mut();
    }
    let c = g_malloc0(size_of::<HxChatMedia>()) as *mut HxChatMedia;
    (*c).id_len = (*m).id_len;
    if (*m).id_len != 0 {
        let dst = g_malloc((*m).id_len) as *mut u8;
        ptr::copy_nonoverlapping((*m).id as *const u8, dst, (*m).id_len);
        (*c).id = dst;
    }
    (*c).mime_len = (*m).mime_len;
    if !(*m).mime.is_null() {
        (*c).mime = g_strndup((*m).mime, (*m).mime_len);
    }
    (*c).width = (*m).width;
    (*c).height = (*m).height;
    (*c).bytes = (*m).bytes;
    (*c).width_present = (*m).width_present;
    (*c).height_present = (*m).height_present;
    (*c).bytes_present = (*m).bytes_present;
    c
}

/// Free an `HxChatMedia` (private; mirrors the C static
/// `hx_chat_media_free` — which the C side keeps for `attach_media`).
///
/// # Safety
/// `m` is NULL or a valid glib-owned `HxChatMedia*`.
unsafe fn media_free(m: *mut HxChatMedia) {
    if m.is_null() {
        return;
    }
    g_free((*m).id as *mut c_void);
    g_free((*m).mime as *mut c_void);
    g_free(m as *mut c_void);
}

/// Boxed copy func. Mirrors the deleted C `hx_chat_event_copy`: shallow
/// copy the scalar fields, then deep-copy `line` (`g_strndup`) and
/// `media` (`media_copy`).
///
/// # Safety
/// `e` is NULL or a valid `HxChatEvent*` with glib-owned `line`/`media`.
#[no_mangle]
pub unsafe extern "C" fn hx_chat_event_copy(e: *mut HxChatEvent) -> *mut HxChatEvent {
    if e.is_null() {
        return ptr::null_mut();
    }
    let c = g_malloc0(size_of::<HxChatEvent>()) as *mut HxChatEvent;
    (*c).cid = (*e).cid;
    (*c).line_len = (*e).line_len;
    (*c).sender_off = (*e).sender_off;
    (*c).sender_len = (*e).sender_len;
    (*c).body_off = (*e).body_off;
    (*c).body_len = (*e).body_len;
    (*c).is_info = (*e).is_info;
    (*c).is_self = (*e).is_self;
    (*c).line = g_strndup((*e).line, (*e).line_len);
    (*c).media = media_copy((*e).media);
    c
}

/// Boxed free func. Mirrors the deleted C `hx_chat_event_free`.
///
/// # Safety
/// `e` is NULL or a valid `HxChatEvent*` with glib-owned `line`/`media`.
#[no_mangle]
pub unsafe extern "C" fn hx_chat_event_free(e: *mut HxChatEvent) {
    if e.is_null() {
        return;
    }
    g_free((*e).line as *mut c_void);
    media_free((*e).media);
    g_free(e as *mut c_void);
}

/// `GBoxedCopyFunc`-shaped shim: matches `unsafe extern "C" fn(gpointer)
/// -> gpointer` exactly and delegates to the typed [`hx_chat_event_copy`],
/// so the boxed-type registration needs no `transmute`.
///
/// # Safety
/// `p` is NULL or a valid `HxChatEvent*`.
unsafe extern "C" fn boxed_copy(p: *mut c_void) -> *mut c_void {
    hx_chat_event_copy(p as *mut HxChatEvent) as *mut c_void
}

/// `GBoxedFreeFunc`-shaped shim — see [`boxed_copy`].
///
/// # Safety
/// `p` is NULL or a valid `HxChatEvent*`.
unsafe extern "C" fn boxed_free(p: *mut c_void) {
    hx_chat_event_free(p as *mut HxChatEvent);
}

/// `HX_TYPE_CHAT_EVENT` accessor — the C ABI the old
/// `G_DEFINE_BOXED_TYPE (HxChatEvent, hx_chat_event, …)` exported.
#[no_mangle]
pub extern "C" fn hx_chat_event_get_type() -> GType {
    static TYPE: OnceLock<usize> = OnceLock::new();
    unsafe { register_once(&TYPE, c"HxChatEvent".as_ptr(), Some(boxed_copy), Some(boxed_free)) }
}

#[cfg(test)]
mod tests {
    use super::*;

    unsafe fn cstr(p: *const c_char, len: usize) -> String {
        String::from_utf8_lossy(std::slice::from_raw_parts(p as *const u8, len)).into_owned()
    }

    /// Build a heap `HxChatEvent` the way a C producer would.
    unsafe fn make_event(cid: u32, line: &str, with_media: bool) -> *mut HxChatEvent {
        let e = g_malloc0(size_of::<HxChatEvent>()) as *mut HxChatEvent;
        (*e).cid = cid;
        (*e).line = g_strndup(line.as_ptr() as *const c_char, line.len());
        (*e).line_len = line.len();
        // Pretend "nick: body" split with nick = first 4 bytes.
        (*e).sender_off = 0;
        (*e).sender_len = 4;
        (*e).body_off = 6;
        (*e).body_len = line.len().saturating_sub(6);
        (*e).is_self = 1;
        if with_media {
            let m = g_malloc0(size_of::<HxChatMedia>()) as *mut HxChatMedia;
            let id: [u8; 4] = [0xDE, 0xAD, 0xBE, 0xEF];
            (*m).id_len = 4;
            let dst = g_malloc(4) as *mut u8;
            ptr::copy_nonoverlapping(id.as_ptr(), dst, 4);
            (*m).id = dst;
            (*m).mime = g_strndup(b"image/png".as_ptr() as *const c_char, 9);
            (*m).mime_len = 9;
            (*m).width = 800;
            (*m).height = 600;
            (*m).width_present = 1;
            (*m).height_present = 1;
            (*e).media = m;
        }
        e
    }

    #[test]
    fn get_type_is_a_registered_boxed_type() {
        let t = hx_chat_event_get_type();
        assert_ne!(t, 0);
        assert_eq!(t, hx_chat_event_get_type());
        let ty: glib::Type = unsafe { glib::translate::from_glib(t) };
        assert!(ty.is_a(glib::Type::BOXED));
    }

    #[test]
    fn copy_deep_copies_line_and_media() {
        unsafe {
            let a = make_event(7, "alice: hello there", true);
            let b = hx_chat_event_copy(a);
            assert_ne!(a, b);
            // line is a distinct allocation with the same bytes.
            assert_ne!((*a).line, (*b).line);
            assert_eq!(cstr((*b).line, (*b).line_len), "alice: hello there");
            // scalar fields carried.
            assert_eq!((*b).cid, 7);
            assert_eq!((*b).sender_len, 4);
            assert_eq!((*b).body_off, 6);
            assert_eq!((*b).is_self, 1);
            // media deep-copied: distinct struct + distinct id/mime.
            assert!(!(*b).media.is_null());
            assert_ne!((*a).media, (*b).media);
            let ma = &*(*a).media;
            let mb = &*(*b).media;
            assert_ne!(ma.id, mb.id);
            assert_ne!(ma.mime, mb.mime);
            assert_eq!(mb.id_len, 4);
            assert_eq!(std::slice::from_raw_parts(mb.id, 4), &[0xDE, 0xAD, 0xBE, 0xEF]);
            assert_eq!(cstr(mb.mime, mb.mime_len), "image/png");
            assert_eq!(mb.width, 800);
            assert_eq!(mb.height_present, 1);
            // Free original; copy must remain intact.
            hx_chat_event_free(a);
            assert_eq!(cstr((*b).line, (*b).line_len), "alice: hello there");
            assert_eq!(cstr((*(*b).media).mime, (*(*b).media).mime_len), "image/png");
            hx_chat_event_free(b);
        }
    }

    #[test]
    fn copy_handles_media_absent() {
        unsafe {
            let a = make_event(0, "no media here", false);
            let b = hx_chat_event_copy(a);
            assert!((*b).media.is_null());
            assert_eq!(cstr((*b).line, (*b).line_len), "no media here");
            hx_chat_event_free(a);
            hx_chat_event_free(b);
        }
    }

    #[test]
    fn copy_and_free_are_null_safe() {
        unsafe {
            assert!(hx_chat_event_copy(ptr::null_mut()).is_null());
            hx_chat_event_free(ptr::null_mut());
        }
    }

    #[test]
    fn g_boxed_copy_roundtrips_through_the_registered_funcs() {
        unsafe {
            let t = hx_chat_event_get_type();
            let a = make_event(3, "bob: hi", true);
            let b = glib::gobject_ffi::g_boxed_copy(t, a as *mut c_void) as *mut HxChatEvent;
            assert_ne!(a, b);
            assert_eq!(cstr((*b).line, (*b).line_len), "bob: hi");
            assert_eq!(cstr((*(*b).media).mime, (*(*b).media).mime_len), "image/png");
            glib::gobject_ffi::g_boxed_free(t, b as *mut c_void);
            hx_chat_event_free(a);
        }
    }
}
