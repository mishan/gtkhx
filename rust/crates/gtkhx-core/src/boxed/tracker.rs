//! `HxTrackerServer` + `HxTrackerV3Meta` — tracker-listing value objects
//! (`src/tracker_event.h`, `src/tracker_v3_meta.h`).
//!
//! `HxTrackerV3Meta` is the C-visible form of a v3 record's metadata. It
//! is built here, by [`hx_tracker_v3_meta_new`], from hxproto's
//! [`TrackerMeta`] — the decoder hxd-ng shares — and copied and freed
//! here. The tracker window reads its fields directly. The C header keeps
//! the struct definition for `tracker_event.c`, with `_Static_assert`s
//! pinning the layout the const asserts below pin on this side.
//!
//! `HxTrackerServer`'s `_copy`/`_free` deep-copy a `GBytes` (ref/unref)
//! and its meta. Its constructors (`hx_tracker_server_new_v1`/`_v3`) stay
//! in C.

use crate::boxed::register_once;
use glib::ffi::{
    g_bytes_ref, g_bytes_unref, g_free, g_malloc0, g_strdup, g_strndup, GBytes, GType,
};
use hxproto::tracker::TrackerMeta;
use std::ffi::c_char;
use std::mem::{align_of, offset_of, size_of};
use std::os::raw::c_void;
use std::ptr;
use std::sync::OnceLock;

// ======================================================================
// HxTrackerV3Meta.
// ======================================================================

/// HxTrackerV3Maturity vocabulary (0x0205).
pub const MATURITY_GENERAL: i32 = 0;
pub const MATURITY_TEEN: i32 = 1;
pub const MATURITY_MATURE: i32 = 2;
pub const MATURITY_ADULT: i32 = 3;

/// HxTrackerV3Category vocabulary (0x0501).
pub const CATEGORY_UNSPECIFIED: i32 = 0;
pub const CATEGORY_GENERAL: i32 = 1;
pub const CATEGORY_DEVELOPMENT: i32 = 2;
pub const CATEGORY_ARCHIVE: i32 = 3;
pub const CATEGORY_WAREZ: i32 = 4;
pub const CATEGORY_GAMING: i32 = 5;
pub const CATEGORY_MEDIA: i32 = 6;
pub const CATEGORY_EDUCATION: i32 = 7;
pub const CATEGORY_RESEARCH: i32 = 8;
pub const CATEGORY_FILE_SHARING: i32 = 9;
pub const CATEGORY_SOCIAL: i32 = 10;
pub const CATEGORY_SECURITY: i32 = 11;
pub const CATEGORY_CREATIVE: i32 = 12;

/// `struct _HxTrackerV3Meta` (`src/tracker_v3_meta.h`). `gboolean` and the
/// two enums are `i32`. Strings are glib-owned UTF-8, NULL when the field
/// was absent. Numeric fields read 0 when absent; `has_max_users` and
/// `has_timezone_offset` tell 0 from absent where that matters.
#[repr(C)]
pub struct HxTrackerV3Meta {
    pub server_software: *mut c_char, // 0x0200
    pub country_code: *mut c_char,    // 0x0201
    pub region: *mut c_char,          // 0x0202
    pub language: *mut c_char,        // 0x0203
    pub max_users: u16,               // 0x0204
    pub has_max_users: i32,
    pub maturity: i32,            // 0x0205 (HxTrackerV3Maturity)
    pub uptime_secs: u32,         // 0x0206
    pub rules_url: *mut c_char,   // 0x0207
    pub banner_url: *mut c_char,  // 0x0208
    pub icon_url: *mut c_char,    // 0x0209
    pub link_down_mbit: u32,      // 0x020A
    pub link_up_mbit: u32,        // 0x020B
    pub timezone_offset_min: i16, // 0x020C
    pub has_timezone_offset: i32,
    pub contact_url: *mut c_char, // 0x020D
    pub server_launched: u32,     // 0x020E
    pub min_proto_version: u16,   // 0x0210
    pub peak_24h: u16,            // 0x0211
    pub avg_24h: u16,             // 0x0212
    pub tags: *mut c_char,        // 0x0310

    pub protocol_version: u16,      // 0x0300
    pub supports_hope: i32,         // 0x0301
    pub supports_tls: i32,          // 0x0302
    pub tls_port: u16,              // 0x0303
    pub supports_inline_media: i32, // 0x0304
    pub supports_voice: i32,        // 0x0305
    pub supports_large_files: i32,  // 0x0306
    pub supports_ipv6: i32,         // 0x0307
    pub hope_ciphers: *mut c_char,  // 0x0309

    pub news_count: u32,          // 0x0450
    pub msgboard_count: u32,      // 0x0451
    pub files_count: u32,         // 0x0452
    pub total_file_size: u32,     // 0x0453
    pub last_news_timestamp: u32, // 0x0454
    pub last_chat_timestamp: u32, // 0x0455

    pub private_listing: i32,  // 0x0500
    pub listing_category: i32, // 0x0501 (HxTrackerV3Category)
    pub language_strict: i32,  // 0x0502

    pub is_promoted: i32,     // 0x0600
    pub first_seen: u32,      // 0x0601
    pub last_heartbeat: u32,  // 0x0602
    pub verified_online: i32, // 0x0603
}

// Layout pins, matching the _Static_asserts in tracker_event.c.
const _: () = {
    assert!(size_of::<HxTrackerV3Meta>() == 216);
    assert!(align_of::<HxTrackerV3Meta>() == 8);
    assert!(offset_of!(HxTrackerV3Meta, server_software) == 0);
    assert!(offset_of!(HxTrackerV3Meta, country_code) == 8);
    assert!(offset_of!(HxTrackerV3Meta, region) == 16);
    assert!(offset_of!(HxTrackerV3Meta, language) == 24);
    assert!(offset_of!(HxTrackerV3Meta, max_users) == 32);
    assert!(offset_of!(HxTrackerV3Meta, rules_url) == 48);
    assert!(offset_of!(HxTrackerV3Meta, banner_url) == 56);
    assert!(offset_of!(HxTrackerV3Meta, icon_url) == 64);
    assert!(offset_of!(HxTrackerV3Meta, contact_url) == 88);
    assert!(offset_of!(HxTrackerV3Meta, tags) == 112);
    assert!(offset_of!(HxTrackerV3Meta, protocol_version) == 120);
    assert!(offset_of!(HxTrackerV3Meta, hope_ciphers) == 152);
    assert!(offset_of!(HxTrackerV3Meta, verified_online) == 208);
};

impl HxTrackerV3Meta {
    /// The ten owned strings, for copy and free.
    fn strings(&mut self) -> [&mut *mut c_char; 10] {
        [
            &mut self.server_software,
            &mut self.country_code,
            &mut self.region,
            &mut self.language,
            &mut self.rules_url,
            &mut self.banner_url,
            &mut self.icon_url,
            &mut self.contact_url,
            &mut self.tags,
            &mut self.hope_ciphers,
        ]
    }

    /// Borrow a string field as a `&str` (lossy, empty when NULL).
    ///
    /// # Safety
    /// `p` is NULL or a valid NUL-terminated C string that outlives `'a`.
    pub unsafe fn cstr<'a>(p: *const c_char) -> &'a str {
        if p.is_null() {
            return "";
        }
        std::ffi::CStr::from_ptr(p).to_str().unwrap_or("")
    }
}

/// A glib copy of `s`, or NULL. Like the C decoder's `g_strndup`, it ends
/// at the first NUL: a C string can't carry one.
unsafe fn dup(s: &Option<String>) -> *mut c_char {
    match s {
        None => ptr::null_mut(),
        Some(s) => {
            let end = s.find('\0').unwrap_or(s.len());
            g_strndup(s.as_ptr() as *const c_char, end)
        }
    }
}

fn flag(b: bool) -> i32 {
    i32::from(b)
}

/// Decode a v3 record's TLV trailer (`buf`, `len` bytes, `count` fields)
/// into a new meta. NULL when the trailer is malformed, which makes the
/// whole record untrustworthy. `count == 0` gives an all-absent meta,
/// which is also what a v1 record gets.
///
/// # Safety
/// `buf` is NULL or points to `len` readable bytes.
#[no_mangle]
pub unsafe extern "C" fn hx_tracker_v3_meta_new(
    buf: *const u8,
    len: usize,
    count: u16,
) -> *mut HxTrackerV3Meta {
    let bytes: &[u8] = if buf.is_null() {
        if len > 0 {
            return ptr::null_mut();
        }
        &[]
    } else {
        std::slice::from_raw_parts(buf, len)
    };
    let Some(t) = (if count == 0 {
        Some(TrackerMeta::default())
    } else {
        TrackerMeta::decode(bytes, count)
    }) else {
        return ptr::null_mut();
    };
    let m = g_malloc0(size_of::<HxTrackerV3Meta>()) as *mut HxTrackerV3Meta;
    m.write(HxTrackerV3Meta {
        server_software: dup(&t.server_software),
        country_code: dup(&t.country_code),
        region: dup(&t.region),
        language: dup(&t.language),
        max_users: t.max_users.unwrap_or(0),
        has_max_users: flag(t.max_users.is_some()),
        maturity: t.maturity.map_or(MATURITY_GENERAL, |v| v as i32),
        uptime_secs: t.uptime_secs.unwrap_or(0),
        rules_url: dup(&t.rules_url),
        banner_url: dup(&t.banner_url),
        icon_url: dup(&t.icon_url),
        link_down_mbit: t.link_down_mbit.unwrap_or(0),
        link_up_mbit: t.link_up_mbit.unwrap_or(0),
        timezone_offset_min: t.timezone_offset_min.unwrap_or(0),
        has_timezone_offset: flag(t.timezone_offset_min.is_some()),
        contact_url: dup(&t.contact_url),
        server_launched: t.server_launched.unwrap_or(0),
        min_proto_version: t.min_protocol_version.unwrap_or(0),
        peak_24h: t.peak_24h.unwrap_or(0),
        avg_24h: t.avg_24h.unwrap_or(0),
        tags: dup(&t.tags),
        protocol_version: t.protocol_version.unwrap_or(0),
        supports_hope: flag(t.supports_hope),
        supports_tls: flag(t.supports_tls),
        tls_port: t.tls_port.unwrap_or(0),
        supports_inline_media: flag(t.supports_inline_media),
        supports_voice: flag(t.supports_voice),
        supports_large_files: flag(t.supports_large_files),
        supports_ipv6: flag(t.supports_ipv6),
        hope_ciphers: dup(&t.hope_ciphers),
        news_count: t.news_count.unwrap_or(0),
        msgboard_count: t.msgboard_count.unwrap_or(0),
        files_count: t.files_count.unwrap_or(0),
        total_file_size: t.total_file_size.unwrap_or(0),
        last_news_timestamp: t.last_news_time.unwrap_or(0),
        last_chat_timestamp: t.last_chat_time.unwrap_or(0),
        private_listing: flag(t.private_listing),
        listing_category: t
            .listing_category
            .map_or(CATEGORY_UNSPECIFIED, |v| v as i32),
        language_strict: flag(t.language_strict),
        is_promoted: flag(t.is_promoted),
        first_seen: t.first_seen.unwrap_or(0),
        last_heartbeat: t.last_heartbeat.unwrap_or(0),
        verified_online: flag(t.verified_online),
    });
    m
}

/// Deep-copy an `HxTrackerV3Meta`.
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
    // Every scalar as-is, then each owned string replaced with a copy of
    // its own (g_strdup(NULL) is NULL).
    ptr::copy_nonoverlapping(src, c, 1);
    for s in (*c).strings() {
        *s = g_strdup(*s);
    }
    c
}

/// Free an `HxTrackerV3Meta`.
///
/// # Safety
/// `m` is NULL or a valid glib-owned `HxTrackerV3Meta*`.
#[no_mangle]
pub unsafe extern "C" fn hx_tracker_v3_meta_free(m: *mut HxTrackerV3Meta) {
    if m.is_null() {
        return;
    }
    for s in (*m).strings() {
        g_free(*s as *mut c_void);
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
        register_once(
            &TYPE,
            c"HxTrackerServer".as_ptr(),
            Some(boxed_copy),
            Some(boxed_free),
        )
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

    /// A meta with two of its ten strings set, to prove copy deep-copies
    /// the strings and leaves NULLs NULL.
    unsafe fn make_meta() -> *mut HxTrackerV3Meta {
        let m = hx_tracker_v3_meta_new(ptr::null(), 0, 0);
        assert!(!m.is_null());
        (*m).server_software = g_strdup(c"mhxd 2.0".as_ptr());
        (*m).hope_ciphers = g_strdup(c"chacha20".as_ptr());
        (*m).tls_port = 5600;
        m
    }

    #[test]
    fn meta_copy_deep_copies_strings() {
        unsafe {
            let a = make_meta();
            let b = hx_tracker_v3_meta_copy(a);
            assert!(!b.is_null());
            assert_ne!(a, b);
            assert_ne!((*a).server_software, (*b).server_software);
            assert_eq!(cstr((*b).server_software), "mhxd 2.0");
            assert_eq!(cstr((*b).hope_ciphers), "chacha20");
            assert_eq!((*b).tls_port, 5600);
            assert!((*b).country_code.is_null());
            hx_tracker_v3_meta_free(a);
            assert_eq!(cstr((*b).server_software), "mhxd 2.0"); // copy intact
            hx_tracker_v3_meta_free(b);
        }
    }

    /// One TLV entry in wire form.
    fn tlv(id: u16, value: &[u8]) -> Vec<u8> {
        let mut v = id.to_be_bytes().to_vec();
        v.extend_from_slice(&(value.len() as u16).to_be_bytes());
        v.extend_from_slice(value);
        v
    }

    #[test]
    fn meta_new_fills_the_c_struct() {
        let blob = [
            tlv(0x0200, b"hxd/2.0"),
            tlv(0x0204, &[0, 0]),
            tlv(0x0205, &[3]),
            tlv(0x020c, &(-90i16).to_be_bytes()),
            tlv(0x0300, &190u16.to_be_bytes()),
            tlv(0x0302, &[1]),
            tlv(0x0303, &5600u16.to_be_bytes()),
            tlv(0x0501, &[99]),
            tlv(0x0600, &[1]),
            tlv(0x0201, b""),
        ]
        .concat();
        unsafe {
            let m = hx_tracker_v3_meta_new(blob.as_ptr(), blob.len(), 10);
            assert!(!m.is_null());
            assert_eq!(cstr((*m).server_software), "hxd/2.0");
            assert_eq!(((*m).max_users, (*m).has_max_users), (0, 1));
            assert_eq!((*m).maturity, MATURITY_ADULT);
            assert_eq!(
                ((*m).timezone_offset_min, (*m).has_timezone_offset),
                (-90, 1)
            );
            assert_eq!((*m).protocol_version, 190);
            assert_eq!(((*m).supports_tls, (*m).tls_port), (1, 5600));
            assert_eq!((*m).listing_category, CATEGORY_UNSPECIFIED, "clamped");
            assert_eq!((*m).is_promoted, 1);
            assert_eq!(cstr((*m).country_code), "", "present but empty is not NULL");
            assert!((*m).region.is_null(), "absent is NULL");
            assert_eq!((*m).has_max_users + (*m).supports_voice, 1);
            hx_tracker_v3_meta_free(m);
        }
    }

    #[test]
    fn meta_new_refuses_a_malformed_trailer() {
        let blob = tlv(0x0202, b"Oslo");
        unsafe {
            assert!(hx_tracker_v3_meta_new(blob.as_ptr(), blob.len(), 2).is_null());
            assert!(hx_tracker_v3_meta_new(blob.as_ptr(), blob.len() - 1, 1).is_null());
            assert!(hx_tracker_v3_meta_new(ptr::null(), 4, 1).is_null());
            let empty = hx_tracker_v3_meta_new(ptr::null(), 0, 0);
            assert!(!empty.is_null(), "no trailer is an all-absent meta");
            assert_eq!((*empty).has_max_users, 0);
            hx_tracker_v3_meta_free(empty);
        }
    }

    #[test]
    fn meta_strings_stop_at_a_nul() {
        let blob = tlv(0x0202, b"Os\0lo");
        unsafe {
            let m = hx_tracker_v3_meta_new(blob.as_ptr(), blob.len(), 1);
            assert_eq!(cstr((*m).region), "Os");
            hx_tracker_v3_meta_free(m);
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
            (*e).tlv_bytes = glib::ffi::g_bytes_new(data.as_ptr() as *const c_void, data.len());
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
            assert_eq!(cstr((*(*b).meta).server_software), "mhxd 2.0");
            hx_tracker_server_free(a);
            // After freeing the original, the copy's owned strings + the
            // still-ref'd GBytes + meta remain valid.
            assert_eq!(cstr((*b).address), "203.0.113.42");
            assert_eq!(cstr((*(*b).meta).server_software), "mhxd 2.0");
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
