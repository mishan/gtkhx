//! Typed `#[repr(C)]` mirror of `struct _HxTrackerV3Meta`
//! (`src/tracker_v3_meta.h`).
//!
//! `gtkhx-core::boxed` also mirrors this struct, but only as an opaque
//! 216-byte buffer (it only needs the ten owned-string byte offsets for
//! its `_copy`/`_free`). The tracker window needs to *read* the typed
//! fields — Country / Caps columns, the details dialog's ~40 rows — so
//! this crate carries the full field-by-field mirror. Layout is pinned
//! by the const asserts below; the ten string offsets match
//! `gtkhx-core::boxed`'s `META_STRING_OFFSETS`, and the total size (216) /
//! alignment (8) match the `_Static_assert`s in `tracker_v3_meta.c`.
//!
//! The C producers (`hx_tracker_v3_meta_new`, the wire parser) still
//! fill these structs; this crate only reads them and calls the
//! `gtkhx-core::boxed` `_copy`/`_free` funcs (resolved at final link).

use std::ffi::c_char;
use std::mem::{align_of, offset_of, size_of};

/// HxTrackerV3Maturity vocabulary (0x0205). See `tracker_v3_meta.h`.
pub const MATURITY_GENERAL: i32 = 0;
pub const MATURITY_TEEN: i32 = 1;
pub const MATURITY_MATURE: i32 = 2;
pub const MATURITY_ADULT: i32 = 3;

/// HxTrackerV3Category vocabulary (0x0501). See `tracker_v3_meta.h`.
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

/// Typed mirror of `struct _HxTrackerV3Meta`. `gboolean` → `i32`
/// (compare `!= 0`); the two enums → `i32`. Field order and padding
/// reproduce the C struct exactly (verified: 216 bytes, align 8, the
/// ten `char*` at offsets 0/8/16/24/48/56/64/88/112/152).
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

// Layout pins. Total size + alignment match tracker_v3_meta.c's
// _Static_asserts; the ten string offsets match gtkhx-core::boxed's
// META_STRING_OFFSETS = [0, 8, 16, 24, 48, 56, 64, 88, 112, 152].
const _: () = {
    assert!(size_of::<HxTrackerV3Meta>() == 216);
    assert!(align_of::<HxTrackerV3Meta>() == 8);
    assert!(offset_of!(HxTrackerV3Meta, server_software) == 0);
    assert!(offset_of!(HxTrackerV3Meta, country_code) == 8);
    assert!(offset_of!(HxTrackerV3Meta, region) == 16);
    assert!(offset_of!(HxTrackerV3Meta, language) == 24);
    assert!(offset_of!(HxTrackerV3Meta, rules_url) == 48);
    assert!(offset_of!(HxTrackerV3Meta, banner_url) == 56);
    assert!(offset_of!(HxTrackerV3Meta, icon_url) == 64);
    assert!(offset_of!(HxTrackerV3Meta, contact_url) == 88);
    assert!(offset_of!(HxTrackerV3Meta, tags) == 112);
    assert!(offset_of!(HxTrackerV3Meta, hope_ciphers) == 152);
    // A couple of scalar offsets too, so a mis-sized gboolean/enum
    // (e.g. someone "fixing" a field to u8) trips the build.
    assert!(offset_of!(HxTrackerV3Meta, max_users) == 32);
    assert!(offset_of!(HxTrackerV3Meta, protocol_version) == 120);
    assert!(offset_of!(HxTrackerV3Meta, verified_online) == 208);
};

impl HxTrackerV3Meta {
    /// Borrow a `*const c_char` field as a `&str` (lossy, empty when
    /// NULL). Lifetime is tied to the meta; only call while the row
    /// that owns this meta is alive.
    ///
    /// # Safety
    /// `p` is NULL or a valid NUL-terminated UTF-8 C string owned by
    /// this meta.
    pub unsafe fn cstr<'a>(p: *const c_char) -> &'a str {
        if p.is_null() {
            return "";
        }
        std::ffi::CStr::from_ptr(p).to_str().unwrap_or("")
    }
}

/// Build the compact "Caps" column string from a meta pointer. Mirrors
/// the C `format_caps_badges`: `★` (promoted), `HOPE`, `TLS`, `v6`,
/// space-separated in that fixed order. Empty string when `m` is NULL
/// or advertises nothing.
///
/// # Safety
/// `m` is NULL or a valid `HxTrackerV3Meta*`.
pub unsafe fn caps_badges(m: *const HxTrackerV3Meta) -> String {
    if m.is_null() {
        return String::new();
    }
    let m = &*m;
    let mut out = String::new();
    fn push(out: &mut String, s: &str) {
        if !out.is_empty() {
            out.push(' ');
        }
        out.push_str(s);
    }
    if m.is_promoted != 0 {
        out.push('\u{2605}'); // ★ BLACK STAR
    }
    if m.supports_hope != 0 {
        push(&mut out, "HOPE");
    }
    if m.supports_tls != 0 {
        push(&mut out, "TLS");
    }
    if m.supports_ipv6 != 0 {
        push(&mut out, "v6");
    }
    out
}
