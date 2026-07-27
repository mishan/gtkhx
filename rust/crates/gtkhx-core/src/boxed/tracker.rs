//! `HxTrackerServer` + `HxTrackerV3Meta` — tracker-listing value objects
//! (`src/tracker_event.h`, `src/tracker_v3_meta.h`). R4.2b.
//!
//! `HxTrackerServer`'s `_copy`/`_free` deep-copy a `GBytes` (ref/unref)
//! and an `HxTrackerV3Meta*` — whose own copy/free move here too, so
//! this crate stays self-contained (no undefined externs). The C
//! producers (`hx_tracker_server_new_v1`/`_v3`, `hx_tracker_v3_meta_new`)
//! and the wire parser stay in C and keep filling these `#[repr(C)]`
//! structs.
//!
//! `HxTrackerV3Meta` is a ~40-field struct, but its copy/free only touch
//! its ten owned `char*` strings (the C code did `*c = *src` then
//! `g_strdup` each). So rather than mirror all 40 fields, we treat it as
//! an opaque, correctly-sized+aligned buffer and fix up the ten string
//! pointers by byte offset — far less error-prone than transcribing
//! every scalar field. Size + the ten offsets are pinned by
//! `_Static_assert`s in `tracker_v3_meta.c`.

use crate::boxed::register_once;
use glib::ffi::{g_bytes_ref, g_bytes_unref, g_free, g_malloc0, g_strdup, GBytes, GType};
use std::ffi::c_char;
use std::mem::{align_of, offset_of, size_of};
use std::os::raw::c_void;
use std::ptr;
use std::sync::OnceLock;

// ======================================================================
// HxTrackerV3Meta — opaque buffer + the ten owned-string byte offsets.
// ======================================================================

/// Opaque, layout-faithful stand-in for `struct _HxTrackerV3Meta`. We
/// never name its scalar fields from Rust — only its ten `char*`
/// strings, by byte offset (see [`META_STRING_OFFSETS`]). Size and
/// alignment are pinned against `tracker_v3_meta.c`.
#[repr(C, align(8))]
pub struct HxTrackerV3Meta {
    _opaque: [u8; 216],
}

const _: () = {
    assert!(size_of::<HxTrackerV3Meta>() == 216);
    assert!(align_of::<HxTrackerV3Meta>() == 8);
};

/// Byte offsets of the ten owned `char*` fields within
/// `HxTrackerV3Meta`, in declaration order: server_software,
/// country_code, region, language, rules_url, banner_url, icon_url,
/// contact_url, tags, hope_ciphers. Pinned by `_Static_assert`s in
/// `tracker_v3_meta.c`.
const META_STRING_OFFSETS: [usize; 10] = [0, 8, 16, 24, 48, 56, 64, 88, 112, 152];

#[inline]
unsafe fn str_field(p: *mut HxTrackerV3Meta, off: usize) -> *mut *mut c_char {
    (p as *mut u8).add(off) as *mut *mut c_char
}

/// Deep-copy an `HxTrackerV3Meta` — mirrors the deleted C
/// `hx_tracker_v3_meta_copy` (`*c = *src` then `g_strdup` each string).
///
/// # Safety
/// `src` is NULL or a valid glib-owned `HxTrackerV3Meta*`.
#[no_mangle]
pub unsafe extern "C" fn hx_tracker_v3_meta_copy(
    src: *mut HxTrackerV3Meta,
) -> *mut HxTrackerV3Meta {
    if src.is_null() {
        return ptr::null_mut();
    }
    let c = g_malloc0(size_of::<HxTrackerV3Meta>()) as *mut HxTrackerV3Meta;
    // Shallow-copy the whole struct (all the scalar fields), then
    // overwrite the owned string pointers with deep copies.
    ptr::copy_nonoverlapping(src as *const u8, c as *mut u8, size_of::<HxTrackerV3Meta>());
    for &off in &META_STRING_OFFSETS {
        // g_strdup(NULL) → NULL, matching the C g_strdup of an absent
        // (NULL) optional string field.
        *str_field(c, off) = g_strdup(*str_field(src, off));
    }
    c
}

/// Free an `HxTrackerV3Meta` — mirrors the deleted C
/// `hx_tracker_v3_meta_free`. Still called from C (`hx_tracker_v3_meta
/// _new`'s error path), now resolved against this crate.
///
/// # Safety
/// `m` is NULL or a valid glib-owned `HxTrackerV3Meta*`.
#[no_mangle]
pub unsafe extern "C" fn hx_tracker_v3_meta_free(m: *mut HxTrackerV3Meta) {
    if m.is_null() {
        return;
    }
    for &off in &META_STRING_OFFSETS {
        g_free(*str_field(m, off) as *mut c_void);
    }
    g_free(m as *mut c_void);
}

// ======================================================================
// HxTrackerServer.
// ======================================================================

/// `#[repr(C)]` mirror of `struct _HxTrackerServer`
/// (`src/tracker_event.h`).
#[repr(C)]
pub struct HxTrackerServer {
    pub addr_type: u8,
    pub address: *mut c_char,
    pub port: u16,
    pub nusers: u16,
    pub name: *mut c_char,
    pub desc: *mut c_char,
    pub tlv_count: u16,
    pub tlv_bytes: *mut GBytes,
    pub meta: *mut HxTrackerV3Meta,
    pub total: i32,
}

const _: () = {
    assert!(size_of::<HxTrackerServer>() == 72);
    assert!(offset_of!(HxTrackerServer, addr_type) == 0);
    assert!(offset_of!(HxTrackerServer, address) == 8);
    assert!(offset_of!(HxTrackerServer, port) == 16);
    assert!(offset_of!(HxTrackerServer, nusers) == 18);
    assert!(offset_of!(HxTrackerServer, name) == 24);
    assert!(offset_of!(HxTrackerServer, desc) == 32);
    assert!(offset_of!(HxTrackerServer, tlv_count) == 40);
    assert!(offset_of!(HxTrackerServer, tlv_bytes) == 48);
    assert!(offset_of!(HxTrackerServer, meta) == 56);
    assert!(offset_of!(HxTrackerServer, total) == 64);
};

/// `g_strdup` with the C `x ? x : ""` guarantee: a NULL input yields a
/// freshly-owned empty string, never NULL (the C copy did
/// `g_strdup (e->address ? e->address : "")`).
#[inline]
unsafe fn g_strdup_or_empty(p: *const c_char) -> *mut c_char {
    if p.is_null() {
        g_strdup(c"".as_ptr())
    } else {
        g_strdup(p)
    }
}

/// Boxed copy func — mirrors the deleted C `hx_tracker_server_copy`.
///
/// # Safety
/// `e` is NULL or a valid glib-owned `HxTrackerServer*`.
#[no_mangle]
pub unsafe extern "C" fn hx_tracker_server_copy(e: *mut HxTrackerServer) -> *mut HxTrackerServer {
    if e.is_null() {
        return ptr::null_mut();
    }
    let c = g_malloc0(size_of::<HxTrackerServer>()) as *mut HxTrackerServer;
    (*c).addr_type = (*e).addr_type;
    (*c).address = g_strdup_or_empty((*e).address);
    (*c).port = (*e).port;
    (*c).nusers = (*e).nusers;
    (*c).name = g_strdup_or_empty((*e).name);
    (*c).desc = g_strdup_or_empty((*e).desc);
    (*c).tlv_count = (*e).tlv_count;
    (*c).tlv_bytes = if (*e).tlv_bytes.is_null() {
        ptr::null_mut()
    } else {
        g_bytes_ref((*e).tlv_bytes)
    };
    (*c).meta = hx_tracker_v3_meta_copy((*e).meta);
    (*c).total = (*e).total;
    c
}

/// Boxed free func — mirrors the deleted C `hx_tracker_server_free`.
///
/// # Safety
/// `e` is NULL or a valid glib-owned `HxTrackerServer*`.
#[no_mangle]
pub unsafe extern "C" fn hx_tracker_server_free(e: *mut HxTrackerServer) {
    if e.is_null() {
        return;
    }
    g_free((*e).address as *mut c_void);
    g_free((*e).name as *mut c_void);
    g_free((*e).desc as *mut c_void);
    if !(*e).tlv_bytes.is_null() {
        g_bytes_unref((*e).tlv_bytes);
    }
    hx_tracker_v3_meta_free((*e).meta);
    g_free(e as *mut c_void);
}

/// `GBoxedCopyFunc`-shaped shim: matches `unsafe extern "C" fn(gpointer)
/// -> gpointer` exactly and delegates to the typed
/// [`hx_tracker_server_copy`], so the registration needs no `transmute`.
///
/// # Safety
/// `p` is NULL or a valid `HxTrackerServer*`.
unsafe extern "C" fn boxed_copy(p: *mut c_void) -> *mut c_void {
    hx_tracker_server_copy(p as *mut HxTrackerServer) as *mut c_void
}

/// `GBoxedFreeFunc`-shaped shim — see [`boxed_copy`].
///
/// # Safety
/// `p` is NULL or a valid `HxTrackerServer*`.
unsafe extern "C" fn boxed_free(p: *mut c_void) {
    hx_tracker_server_free(p as *mut HxTrackerServer);
}

/// `HX_TYPE_TRACKER_SERVER` accessor — the C ABI the old
/// `G_DEFINE_BOXED_TYPE (HxTrackerServer, hx_tracker_server, …)` exported.
#[no_mangle]
pub extern "C" fn hx_tracker_server_get_type() -> GType {
    static TYPE: OnceLock<usize> = OnceLock::new();
    unsafe {
        register_once(&TYPE, c"HxTrackerServer".as_ptr(), Some(boxed_copy), Some(boxed_free))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    unsafe fn cstr(p: *const c_char) -> String {
        if p.is_null() {
            return String::from("<null>");
        }
        std::ffi::CStr::from_ptr(p).to_string_lossy().into_owned()
    }

    /// A meta with two of its ten strings set, to prove the offset-based
    /// fixup deep-copies the right fields and leaves NULLs NULL.
    unsafe fn make_meta() -> *mut HxTrackerV3Meta {
        let m = g_malloc0(size_of::<HxTrackerV3Meta>()) as *mut HxTrackerV3Meta;
        assert!(!m.is_null(), "g_malloc0 returned NULL");
        *str_field(m, 0) = g_strdup(c"mhxd 2.0".as_ptr()); // server_software
        *str_field(m, 152) = g_strdup(c"chacha20".as_ptr()); // hope_ciphers
        m
    }

    #[test]
    fn meta_copy_deep_copies_only_string_fields() {
        unsafe {
            let a = make_meta();
            assert!(!a.is_null());
            let b = hx_tracker_v3_meta_copy(a);
            assert!(!b.is_null());
            assert_ne!(a, b);
            // server_software + hope_ciphers deep-copied (distinct ptr, same text).
            assert_ne!(*str_field(a, 0), *str_field(b, 0));
            assert_eq!(cstr(*str_field(b, 0)), "mhxd 2.0");
            assert_eq!(cstr(*str_field(b, 152)), "chacha20");
            // An untouched string field stays NULL in the copy.
            assert!((*str_field(b, 8)).is_null());
            hx_tracker_v3_meta_free(a);
            assert_eq!(cstr(*str_field(b, 0)), "mhxd 2.0"); // copy intact
            hx_tracker_v3_meta_free(b);
        }
    }

    unsafe fn make_server(with_meta: bool, with_tlv: bool) -> *mut HxTrackerServer {
        let e = g_malloc0(size_of::<HxTrackerServer>()) as *mut HxTrackerServer;
        assert!(!e.is_null(), "g_malloc0 returned NULL");
        (*e).addr_type = 0x04;
        (*e).address = g_strdup(c"203.0.113.42".as_ptr());
        (*e).port = 5500;
        (*e).nusers = 12;
        (*e).name = g_strdup(c"Test Server".as_ptr());
        (*e).desc = g_strdup(c"a server".as_ptr());
        (*e).total = 7;
        if with_tlv {
            let data: [u8; 3] = [1, 2, 3];
            (*e).tlv_count = 1;
            (*e).tlv_bytes =
                glib::ffi::g_bytes_new(data.as_ptr() as *const c_void, data.len());
        }
        if with_meta {
            (*e).meta = make_meta();
        }
        e
    }

    #[test]
    fn get_type_is_a_registered_boxed_type() {
        let t = hx_tracker_server_get_type();
        assert_ne!(t, 0);
        assert_eq!(t, hx_tracker_server_get_type());
        let ty: glib::Type = unsafe { glib::translate::from_glib(t) };
        assert!(ty.is_a(glib::Type::BOXED));
    }

    #[test]
    fn server_copy_deep_copies_strings_bytes_and_meta() {
        unsafe {
            let a = make_server(true, true);
            assert!(!a.is_null());
            let b = hx_tracker_server_copy(a);
            assert!(!b.is_null());
            assert_ne!(a, b);
            assert_eq!((*b).addr_type, 0x04);
            assert_eq!((*b).port, 5500);
            assert_eq!((*b).nusers, 12);
            assert_eq!((*b).total, 7);
            assert_ne!((*a).address, (*b).address);
            assert_eq!(cstr((*b).address), "203.0.113.42");
            assert_eq!(cstr((*b).name), "Test Server");
            assert_eq!(cstr((*b).desc), "a server");
            // GBytes is ref-counted: copy shares the same object (ref'd).
            assert_eq!((*a).tlv_bytes, (*b).tlv_bytes);
            // meta deep-copied (distinct allocation).
            assert_ne!((*a).meta, (*b).meta);
            assert_eq!(cstr(*str_field((*b).meta, 0)), "mhxd 2.0");
            hx_tracker_server_free(a);
            // After freeing the original, the copy's owned strings + the
            // still-ref'd GBytes + meta remain valid.
            assert_eq!(cstr((*b).address), "203.0.113.42");
            assert_eq!(cstr(*str_field((*b).meta, 0)), "mhxd 2.0");
            hx_tracker_server_free(b);
        }
    }

    #[test]
    fn server_copy_handles_null_optionals() {
        unsafe {
            // No meta, no tlv, and a NULL address → "" (never NULL).
            let e = g_malloc0(size_of::<HxTrackerServer>()) as *mut HxTrackerServer;
            assert!(!e.is_null());
            (*e).addr_type = 0x48;
            let b = hx_tracker_server_copy(e);
            assert!(!b.is_null());
            assert!(!(*b).address.is_null());
            assert_eq!(cstr((*b).address), "");
            assert_eq!(cstr((*b).name), "");
            assert!((*b).tlv_bytes.is_null());
            assert!((*b).meta.is_null());
            hx_tracker_server_free(e);
            hx_tracker_server_free(b);
        }
    }

    #[test]
    fn copy_and_free_are_null_safe() {
        unsafe {
            assert!(hx_tracker_server_copy(ptr::null_mut()).is_null());
            assert!(hx_tracker_v3_meta_copy(ptr::null_mut()).is_null());
            hx_tracker_server_free(ptr::null_mut());
            hx_tracker_v3_meta_free(ptr::null_mut());
        }
    }

    #[test]
    fn g_boxed_copy_roundtrips_through_the_registered_funcs() {
        unsafe {
            let t = hx_tracker_server_get_type();
            let a = make_server(true, false);
            assert!(!a.is_null());
            let b = glib::gobject_ffi::g_boxed_copy(t, a as *mut c_void) as *mut HxTrackerServer;
            assert!(!b.is_null());
            assert_ne!(a, b);
            assert_eq!(cstr((*b).name), "Test Server");
            glib::gobject_ffi::g_boxed_free(t, b as *mut c_void);
            hx_tracker_server_free(a);
        }
    }
}
