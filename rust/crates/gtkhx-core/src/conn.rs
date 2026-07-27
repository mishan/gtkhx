//! `hxconn` — owner of `struct htlc_conn`, the per-connection Hotline session
//! state. Formerly a C struct defined in `protocol.h` and accessed through the
//! thin `hxconn.c` getter/setter seam; this crate is the E1c flip
//! (`docs/rust/network-endgame.md`): the storage and every accessor body move
//! into Rust behind the identical `hx_conn_*` C ABI, so the call sites — which
//! already went through the accessors — don't change.
//!
//! Storage is a plain `#[repr(C)]` struct owned by this crate. Production
//! allocates one per session via `hx_conn_new` (a `Box`, never freed — it lives
//! for the process); the Tier-2/Tier-3 tests stack-allocate the pinned C mirror
//! in `hxconn_layout.h`. The `#[repr(C)]` layout + the `_Static_assert`s in
//! `hxconn_layout.h` (and the size assert below) keep the two in lockstep so a
//! pointer from either side is a valid `*mut HtlcConn`.

use glib::ffi::{gboolean, GFALSE, GTRUE};
use std::os::raw::{c_char, c_int, c_uint, c_void};

/// HOSTLEN from `compat.h`.
const HOSTLEN: usize = 256;

/// `flags` bitfield bits. `visible` (C bit 0) is unused — no accessor — so only
/// the two live flags are named. Because no C code reads `flags` directly (only
/// these accessors), the bit assignment is this crate's alone.
const FLAG_LOGGED_IN: u32 = 1 << 0;
const FLAG_POST_LOGIN_FETCHED: u32 = 1 << 1;

/// `#[repr(C)]` mirror of `struct htlc_conn` (was `protocol.h`). Field order,
/// types, and sizes match the C definition exactly; the layout is pinned by the
/// `_Static_assert`s in `hxconn_layout.h` and the size assert at the bottom of
/// this file.
#[repr(C)]
pub struct HtlcConn {
    sess: *mut c_void,
    serverhost: [c_char; HOSTLEN],
    serverport: u16,
    tls: c_char,
    ip_addr: [c_char; HOSTLEN],
    fd: c_int,
    trans: u32,
    icon: u16,
    uid: u16,
    version: u16,
    /// The C `struct { guint32 visible:1, logged_in:1, post_login_fetched:1,
    /// reserved:29; } flags` — 4 bytes. Held as a plain `u32`; the accessors own
    /// the bit semantics (see `FLAG_*`).
    flags: u32,
    /// 8-byte big-endian access bitmap (`hl_access_bits` = `guint64`). Kept as a
    /// `u64` to match the C alignment; the bit helpers read it via `to_ne_bytes`
    /// so the byte-wise semantics of the old `(guint8 *)&access` view are exact.
    access: u64,
    name: [c_char; 32],
    login: [c_char; 32],
    nick_color: u32,
    cipheralg: [c_char; 32],
    compressalg: [c_char; 32],
    hope_aead: *mut c_void,
    caps: u64,
    history_max_msgs: u32,
    history_max_days: u32,
    media_max_bytes: u32,
    media_max_dimension: u32,
    media_max_pixels: u32,
    media_chunk_size: u32,
    media_max_frames: u32,
    media_max_duration_ms: u32,
    chat_history_last_msgid: u64,
    gif_icons_state: c_int,
    gif_icons_probe_timer: c_uint,
    gif_icons_probe_trans: u32,
}

/// All fields are POD (integers, byte arrays, and null-valid raw pointers), so a
/// zeroed struct is a valid, "fresh" connection — the exact semantics of the old
/// `g_new0` / `memset(0)`.
unsafe fn zeroed() -> HtlcConn {
    std::mem::zeroed()
}

/// g_strlcpy-equivalent: copy up to `dst.len() - 1` bytes from the NUL-terminated
/// `src`, always NUL-terminating. NULL `src` yields an empty string.
unsafe fn strlcpy(dst: &mut [c_char], src: *const c_char) {
    if dst.is_empty() {
        return;
    }
    if src.is_null() {
        dst[0] = 0;
        return;
    }
    let cap = dst.len();
    let mut i = 0;
    while i + 1 < cap {
        let c = *src.add(i);
        if c == 0 {
            break;
        }
        dst[i] = c;
        i += 1;
    }
    dst[i] = 0;
}

/// Zero the whole buffer, then copy `src` if it's a non-empty string. Mirrors the
/// old `memset(0)` + `if (v && *v) g_strlcpy(...)` for cipheralg / compressalg.
unsafe fn set_zeroed_str(dst: &mut [c_char], src: *const c_char) {
    for b in dst.iter_mut() {
        *b = 0;
    }
    if !src.is_null() && *src != 0 {
        strlcpy(dst, src);
    }
}

// ---- Lifecycle ------------------------------------------------------------

/// Allocate a fresh, zeroed connection. Production keeps exactly one for the
/// process lifetime (the single session), so it is intentionally never freed —
/// `hx_conn_free` exists for symmetry / future multi-conn.
#[no_mangle]
pub extern "C" fn hx_conn_new() -> *mut HtlcConn {
    Box::into_raw(Box::new(unsafe { zeroed() }))
}

/// Reset to the just-allocated state (the old reconnect `memset(0)`). The caller
/// re-stamps sess / icon / name afterwards, as `gtkhx.c` always did.
///
/// # Safety
/// `h` must be a valid `*mut HtlcConn` (or NULL, a no-op).
#[no_mangle]
pub unsafe extern "C" fn hx_conn_reset(h: *mut HtlcConn) {
    if h.is_null() {
        return;
    }
    *h = zeroed();
}

/// Free a connection allocated by `hx_conn_new`.
///
/// # Safety
/// `h` must be a `hx_conn_new` pointer not yet freed (or NULL).
#[no_mangle]
pub unsafe extern "C" fn hx_conn_free(h: *mut HtlcConn) {
    if !h.is_null() {
        drop(Box::from_raw(h));
    }
}

// ---- Scalar getter/setter macro ------------------------------------------

macro_rules! scalar {
    ($get:ident, $set:ident, $field:ident, $ty:ty) => {
        #[no_mangle]
        pub unsafe extern "C" fn $get(h: *const HtlcConn) -> $ty {
            (*h).$field
        }
        #[no_mangle]
        pub unsafe extern "C" fn $set(h: *mut HtlcConn, v: $ty) {
            (*h).$field = v;
        }
    };
}

scalar!(hx_conn_serverport, hx_conn_set_serverport, serverport, u16);
scalar!(hx_conn_tls, hx_conn_set_tls, tls, c_char);
scalar!(hx_conn_version, hx_conn_set_version, version, u16);
scalar!(hx_conn_caps, hx_conn_set_caps, caps, u64);
scalar!(hx_conn_uid, hx_conn_set_uid, uid, u16);
scalar!(hx_conn_icon, hx_conn_set_icon, icon, u16);
scalar!(hx_conn_nick_color, hx_conn_set_nick_color, nick_color, u32);
scalar!(hx_conn_fd, hx_conn_set_fd, fd, c_int);
scalar!(hx_conn_trans, hx_conn_set_trans, trans, u32);
scalar!(
    hx_conn_history_max_msgs,
    hx_conn_set_history_max_msgs,
    history_max_msgs,
    u32
);
scalar!(
    hx_conn_history_max_days,
    hx_conn_set_history_max_days,
    history_max_days,
    u32
);
scalar!(
    hx_conn_chat_history_last_msgid,
    hx_conn_set_chat_history_last_msgid,
    chat_history_last_msgid,
    u64
);
scalar!(
    hx_conn_media_max_bytes,
    hx_conn_set_media_max_bytes,
    media_max_bytes,
    u32
);
scalar!(
    hx_conn_media_max_dimension,
    hx_conn_set_media_max_dimension,
    media_max_dimension,
    u32
);
scalar!(
    hx_conn_media_max_pixels,
    hx_conn_set_media_max_pixels,
    media_max_pixels,
    u32
);
scalar!(
    hx_conn_media_chunk_size,
    hx_conn_set_media_chunk_size,
    media_chunk_size,
    u32
);
scalar!(
    hx_conn_media_max_frames,
    hx_conn_set_media_max_frames,
    media_max_frames,
    u32
);
scalar!(
    hx_conn_media_max_duration_ms,
    hx_conn_set_media_max_duration_ms,
    media_max_duration_ms,
    u32
);
scalar!(
    hx_conn_gif_icons_state,
    hx_conn_set_gif_icons_state,
    gif_icons_state,
    c_int
);
scalar!(
    hx_conn_gif_icons_probe_timer,
    hx_conn_set_gif_icons_probe_timer,
    gif_icons_probe_timer,
    c_uint
);
scalar!(
    hx_conn_gif_icons_probe_trans,
    hx_conn_set_gif_icons_probe_trans,
    gif_icons_probe_trans,
    u32
);

/// `guint32 hx_conn_trans_post_inc` — return the current trans, then increment
/// (the hlpack trans-stamp idiom).
#[no_mangle]
pub unsafe extern "C" fn hx_conn_trans_post_inc(h: *mut HtlcConn) -> u32 {
    let t = (*h).trans;
    (*h).trans = t.wrapping_add(1);
    t
}

/// Reset all six inline-media advisory limits to 0 ("use client defaults").
#[no_mangle]
pub unsafe extern "C" fn hx_conn_reset_media_limits(h: *mut HtlcConn) {
    (*h).media_max_bytes = 0;
    (*h).media_max_dimension = 0;
    (*h).media_max_pixels = 0;
    (*h).media_chunk_size = 0;
    (*h).media_max_frames = 0;
    (*h).media_max_duration_ms = 0;
}

// ---- Capability bitmask ---------------------------------------------------

#[no_mangle]
pub unsafe extern "C" fn hx_conn_has_cap(h: *const HtlcConn, cap: u64) -> gboolean {
    if (*h).caps & cap != 0 {
        GTRUE
    } else {
        GFALSE
    }
}

// ---- Login lifecycle flags ------------------------------------------------

macro_rules! flag {
    ($get:ident, $set:ident, $bit:expr) => {
        #[no_mangle]
        pub unsafe extern "C" fn $get(h: *const HtlcConn) -> gboolean {
            if (*h).flags & $bit != 0 {
                GTRUE
            } else {
                GFALSE
            }
        }
        #[no_mangle]
        pub unsafe extern "C" fn $set(h: *mut HtlcConn, v: gboolean) {
            if v != GFALSE {
                (*h).flags |= $bit;
            } else {
                (*h).flags &= !$bit;
            }
        }
    };
}

flag!(hx_conn_logged_in, hx_conn_set_logged_in, FLAG_LOGGED_IN);
flag!(
    hx_conn_post_login_fetched,
    hx_conn_set_post_login_fetched,
    FLAG_POST_LOGIN_FETCHED
);

// ---- Access bitmap --------------------------------------------------------

/// TRUE iff any bit is set — the "server actually sent an access bitmap" proxy.
unsafe fn access_any_set(h: *const HtlcConn) -> bool {
    (*h).access != 0
}

#[no_mangle]
pub unsafe extern "C" fn hx_conn_access_has(h: *const HtlcConn, bit: c_int) -> gboolean {
    if !(0..64).contains(&bit) {
        return GFALSE;
    }
    let bytes = (*h).access.to_ne_bytes();
    if bytes[(bit >> 3) as usize] & (0x80u8 >> (bit & 7)) != 0 {
        GTRUE
    } else {
        GFALSE
    }
}

#[no_mangle]
pub unsafe extern "C" fn hx_conn_access_permits(h: *const HtlcConn, bit: c_int) -> gboolean {
    if !access_any_set(h) || hx_conn_access_has(h, bit) != GFALSE {
        GTRUE
    } else {
        GFALSE
    }
}

/// Copy the 8-byte wire bitmap into `access`, preserving byte order (the old
/// `memcpy(&h->access, bytes, 8)`).
#[no_mangle]
pub unsafe extern "C" fn hx_conn_set_access(h: *mut HtlcConn, bytes: *const u8) {
    let mut b = [0u8; 8];
    std::ptr::copy_nonoverlapping(bytes, b.as_mut_ptr(), 8);
    (*h).access = u64::from_ne_bytes(b);
}

// ---- Strings --------------------------------------------------------------

#[no_mangle]
pub unsafe extern "C" fn hx_conn_serverhost(h: *const HtlcConn) -> *const c_char {
    (*h).serverhost.as_ptr()
}
#[no_mangle]
pub unsafe extern "C" fn hx_conn_set_serverhost(h: *mut HtlcConn, v: *const c_char) {
    strlcpy(&mut (*h).serverhost, v);
}

#[no_mangle]
pub unsafe extern "C" fn hx_conn_ip_addr(h: *const HtlcConn) -> *const c_char {
    (*h).ip_addr.as_ptr()
}
#[no_mangle]
pub unsafe extern "C" fn hx_conn_set_ip_addr(h: *mut HtlcConn, v: *const c_char) {
    strlcpy(&mut (*h).ip_addr, v);
}

#[no_mangle]
pub unsafe extern "C" fn hx_conn_name(h: *const HtlcConn) -> *const c_char {
    (*h).name.as_ptr()
}
#[no_mangle]
pub unsafe extern "C" fn hx_conn_set_name(h: *mut HtlcConn, v: *const c_char) {
    strlcpy(&mut (*h).name, v);
}
/// The writable name buffer for the NICK cfgvar binding (options.c) — a stable
/// `char *` into this connection's storage.
#[no_mangle]
pub unsafe extern "C" fn hx_conn_name_buf(h: *mut HtlcConn) -> *mut c_char {
    (*h).name.as_mut_ptr()
}

#[no_mangle]
pub unsafe extern "C" fn hx_conn_set_login(h: *mut HtlcConn, v: *const c_char) {
    strlcpy(&mut (*h).login, v);
}

#[no_mangle]
pub unsafe extern "C" fn hx_conn_cipheralg(h: *const HtlcConn) -> *const c_char {
    (*h).cipheralg.as_ptr()
}
#[no_mangle]
pub unsafe extern "C" fn hx_conn_set_cipheralg(h: *mut HtlcConn, v: *const c_char) {
    set_zeroed_str(&mut (*h).cipheralg, v);
}

#[no_mangle]
pub unsafe extern "C" fn hx_conn_compressalg(h: *const HtlcConn) -> *const c_char {
    (*h).compressalg.as_ptr()
}
#[no_mangle]
pub unsafe extern "C" fn hx_conn_set_compressalg(h: *mut HtlcConn, v: *const c_char) {
    set_zeroed_str(&mut (*h).compressalg, v);
}

// ---- Icon (value + pointer escape hatch) ----------------------------------

/// The raw address of the icon field for the ICON cfgvar binding (options.c),
/// which needs a stable `guint16 *`. Deliberate escape hatch — the pointer is
/// into this connection's storage (stable under `Box`), valid for its lifetime.
#[no_mangle]
pub unsafe extern "C" fn hx_conn_icon_ptr(h: *mut HtlcConn) -> *mut u16 {
    &mut (*h).icon as *mut u16
}

// ---- Opaque pointers (sess back-pointer, HOPE AEAD handle) ----------------

#[no_mangle]
pub unsafe extern "C" fn hx_conn_sess(h: *const HtlcConn) -> *mut c_void {
    (*h).sess
}
#[no_mangle]
pub unsafe extern "C" fn hx_conn_set_sess(h: *mut HtlcConn, s: *mut c_void) {
    (*h).sess = s;
}

#[no_mangle]
pub unsafe extern "C" fn hx_conn_hope_aead(h: *const HtlcConn) -> *mut c_void {
    (*h).hope_aead
}
#[no_mangle]
pub unsafe extern "C" fn hx_conn_set_hope_aead(h: *mut HtlcConn, p: *mut c_void) {
    (*h).hope_aead = p;
}

/// Pin the layout: if this fires, `HtlcConn` and the C mirror in
/// `hxconn_layout.h` have drifted. The C side pins the same value with
/// `_Static_assert (sizeof (struct htlc_conn) == HXCONN_SIZEOF)`.
pub const HXCONN_SIZEOF: usize = 760;
const _: () = assert!(std::mem::size_of::<HtlcConn>() == HXCONN_SIZEOF);

#[cfg(test)]
mod tests;
