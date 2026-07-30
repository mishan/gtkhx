//! GIF-icon receive path (ported from `rcv.c`).
//!
//! The GIF-icons extension broadcasts an `ICON_CHANGE` frame (uid only) when a
//! user changes their avatar. This crate owns that handler end to end: parse the
//! uid out of the frame body (via `hotline-proto`) and emit the
//! `gif-icon-changed` signal so the user list refreshes the avatar. Unlike the
//! chat-invite handler (whose parse stayed C because it reads `htlc->in` through
//! a struct), the icon-change parse is already a bytes-in Rust parser, so the
//! whole handler moves here and the C side is a one-line forwarder.

use std::os::raw::{c_int, c_uint, c_void};

#[cfg(not(test))]
use gtkhx_core::session::{
    gtkhx_session_emit_gif_icon_changed, gtkhx_session_emit_gif_icon_data,
    gtkhx_session_get_default,
};
#[cfg(not(test))]
use hotline_proto::ffi::{gtkhx_proto_gif_icon_is_gif, gtkhx_proto_parse_icon_change};

// Native reply parsers — pure Rust, identical in test and production. The C rcv
// handlers used to round-trip through the `gtkhx_proto_parse_icon_*` C ABI; here
// we walk the frame with the native `gif_icons` API directly (no FFI bounce).
use hotline_proto::gif_icons::{parse_icon_get_reply, parse_icon_list};
use hotline_proto::wire::ChunkIter;

/// GIF-icons negotiation tri-state (mirror of the C `enum` in `gif_icons.h`).
const GIF_ICONS_SUPPORTED: c_int = 1;
const GIF_ICONS_UNSUPPORTED: c_int = 2;

// The connection negotiation-state accessors (gtkhx-core `#[no_mangle]`), the
// GLib watchdog disarm, and the saved-avatar push are reached over the C ABI;
// the test build shadows each with a recording double at the bottom of the file.
#[cfg(not(test))]
extern "C" {
    fn hx_conn_set_gif_icons_state(h: *mut c_void, v: c_int);
    fn hx_conn_gif_icons_probe_timer(h: *const c_void) -> c_uint;
    fn hx_conn_set_gif_icons_probe_timer(h: *mut c_void, v: c_uint);
    /// GLib `g_source_remove` — disarm the probe watchdog source.
    fn g_source_remove(tag: c_uint) -> c_int;
    /// Push our saved avatar once the server proves capable (`gif_icons.c`).
    fn hx_icon_send_saved(htlc: *mut c_void);
}

/// Borrow the reply frame as a byte slice (empty on a NULL frame).
unsafe fn frame_slice<'a>(frame: *const c_void, frame_len: usize) -> &'a [u8] {
    if frame.is_null() {
        &[]
    } else {
        std::slice::from_raw_parts(frame as *const u8, frame_len)
    }
}

/// True when the reply frame's task-error bit is set — the native equivalent of
/// the C `task_inerror()` (header parse + `flag & 1`). A frame too short to hold
/// a header is treated as not-in-error, matching the C shim.
unsafe fn task_in_error(frame: *const c_void, frame_len: usize) -> bool {
    let s = frame_slice(frame, frame_len);
    hotline_proto::parse::Header::parse(s).is_some_and(|h| h.in_error())
}

/// Disarm the GIF-icons probe watchdog if armed (the reply beat the timeout).
unsafe fn disarm_probe_timer(htlc: *mut c_void) {
    let tag = hx_conn_gif_icons_probe_timer(htlc);
    if tag != 0 {
        g_source_remove(tag);
        hx_conn_set_gif_icons_probe_timer(htlc, 0);
    }
}

/// `void rcv_task_icon_get (htlc, frame, frame_len, uid_ptr, data)` — HTLS reply
/// to `ICON_GET` (1863): `UID` + `ICON_GIF`. A reply arriving at all proves the
/// server speaks the GIF-icons extension, so flip negotiation to SUPPORTED. A
/// `gif_len == 0` result is a valid "avatar cleared" and is still published so
/// the view drops any stale cached avatar (`hx_icon_data_recv` maps empty →
/// `(NULL, 0)`). The uid is echoed in the reply body, so `uid_ptr` is ignored.
///
/// # Safety
/// C-ABI reply callback invoked by `hx_rcv_task` on the main thread. `frame` is
/// valid for `frame_len` bytes; `uid_ptr` / `data` are unused.
#[no_mangle]
pub unsafe extern "C" fn rcv_task_icon_get(
    htlc: *mut c_void,
    frame: *const c_void,
    frame_len: usize,
    _uid_ptr: *mut c_void,
    _data: *mut c_void,
) {
    let s = frame_slice(frame, frame_len);
    let Some(e) = parse_icon_get_reply(ChunkIter::over_message(s, s.len())) else {
        return;
    };
    hx_conn_set_gif_icons_state(htlc, GIF_ICONS_SUPPORTED);
    hx_icon_data_recv(htlc, e.uid, e.gif.as_ptr(), e.gif.len() as u32);
}

/// `void rcv_task_icon_getlist (htlc, frame, frame_len, ptr, data)` — HTLS reply
/// to `ICON_GETLIST` (1861): 0..N packed `ICON_LIST` entries. Also the
/// resolution point for the post-login probe.
///
/// The extension has no capability/access bit and no version tie, so support is
/// detected purely by this probe. An ERROR reply is the "not supported" answer —
/// exactly like the watchdog timing out — so record UNSUPPORTED, disarm the
/// watchdog, and return WITHOUT a user toast (a speculative probe's rejection is
/// expected and non-actionable). Otherwise the server is confirmed capable:
/// record SUPPORTED, disarm the watchdog, push our saved avatar (no-op when none
/// saved), and publish each listed avatar.
///
/// # Safety
/// C-ABI reply callback invoked by `hx_rcv_task` on the main thread. `frame` is
/// valid for `frame_len` bytes; `ptr` / `data` are unused (NULL at register time).
#[no_mangle]
pub unsafe extern "C" fn rcv_task_icon_getlist(
    htlc: *mut c_void,
    frame: *const c_void,
    frame_len: usize,
    _ptr: *mut c_void,
    _data: *mut c_void,
) {
    if task_in_error(frame, frame_len) {
        hx_conn_set_gif_icons_state(htlc, GIF_ICONS_UNSUPPORTED);
        disarm_probe_timer(htlc);
        return;
    }

    hx_conn_set_gif_icons_state(htlc, GIF_ICONS_SUPPORTED);
    disarm_probe_timer(htlc);
    hx_icon_send_saved(htlc);

    // A uid is a u16, so a well-formed list holds at most 65536 entries; clamp
    // the walk so a hostile/duplicated reply can't drive an unbounded emit storm.
    const MAX_ENTRIES: usize = u16::MAX as usize + 1;
    let s = frame_slice(frame, frame_len);
    for (n, e) in parse_icon_list(ChunkIter::over_message(s, s.len())).enumerate() {
        if n >= MAX_ENTRIES {
            break;
        }
        hx_icon_data_recv(htlc, e.uid, e.gif.as_ptr(), e.gif.len() as u32);
    }
}

/// `void hx_icon_data_recv (htlc, uid, gif, len)` — publish a user's GIF avatar
/// bytes (from an `ICON_GET` reply or an `ICON_GETLIST` entry), upholding the
/// `gif-icon-data` signal's "raw GIF bytes or empty" contract:
///
/// - `len == 0` → a cleared avatar; forward `(NULL, 0)` so no subscriber
///   dereferences a possibly-dangling pointer and any stale avatar is dropped.
/// - a non-empty payload that fails the GIF87a/89a signature check is
///   network-supplied garbage (buggy / hostile server) → coerce to cleared
///   `(NULL, 0)` so nothing tries to decode it.
/// - otherwise forward the bytes verbatim.
///
/// # Safety
/// When `len > 0`, `gif` must point to `len` readable bytes; `htlc` is only
/// forwarded to the signal.
#[no_mangle]
pub unsafe extern "C" fn hx_icon_data_recv(htlc: *mut c_void, uid: u16, gif: *const u8, len: u32) {
    let (ptr, out_len): (*const c_void, u32) =
        if len == 0 || !gtkhx_proto_gif_icon_is_gif(gif, len as usize) {
            (std::ptr::null(), 0)
        } else {
            (gif as *const c_void, len)
        };
    gtkhx_session_emit_gif_icon_data(gtkhx_session_get_default(), htlc, uid, ptr, out_len);
}

/// `void hx_icon_change_recv (htlc, buf, len)` — parse an `ICON_CHANGE`
/// broadcast and, if it carries a uid, emit `gif-icon-changed` so the avatar
/// refreshes. A malformed frame (no uid) is dropped silently.
///
/// # Safety
/// When `len > 0`, `buf` must point to `len` readable bytes (`htlc->in.buf` /
/// `htlc->in.pos` at the call site); when `len == 0` the bytes are never read,
/// so `buf` may be null. `htlc` is only forwarded to the signal (never
/// dereferenced here), so it too may be null.
#[no_mangle]
pub unsafe extern "C" fn hx_icon_change_recv(htlc: *mut c_void, buf: *const u8, len: usize) {
    let mut uid: u16 = 0;
    if !gtkhx_proto_parse_icon_change(buf, len, &mut uid) {
        return;
    }
    gtkhx_session_emit_gif_icon_changed(gtkhx_session_get_default(), htlc, uid);
}

// ---- test doubles for the C environment ------------------------------------

#[cfg(test)]
pub(crate) mod test_env {
    use std::cell::Cell;

    thread_local! {
        /// Drives the stubbed parser: `Some(uid)` → parse succeeds with that
        /// uid; `None` → parse fails (malformed frame).
        pub static PARSE_UID: Cell<Option<u16>> = const { Cell::new(None) };
        /// Records the uid of the last emitted gif-icon-changed, or None.
        pub static EMITTED: Cell<Option<u16>> = const { Cell::new(None) };
        /// Drives the stubbed GIF-signature check.
        pub static IS_GIF: Cell<bool> = const { Cell::new(true) };
        /// Records the last emitted gif-icon-data as (uid, ptr_is_null, len).
        pub static DATA_EMITTED: Cell<Option<(u16, bool, u32)>> = const { Cell::new(None) };
        /// Count of gif-icon-data emits (getlist walks multiple entries).
        pub static DATA_COUNT: Cell<u32> = const { Cell::new(0) };
        /// The negotiation state the handler set (0 = untouched).
        pub static STATE: Cell<i32> = const { Cell::new(0) };
        /// Probe watchdog source id: the getter returns it, the setter overwrites.
        pub static PROBE_TIMER: Cell<u32> = const { Cell::new(0) };
        /// The tag passed to g_source_remove, if any.
        pub static SOURCE_REMOVED: Cell<Option<u32>> = const { Cell::new(None) };
        /// True once hx_icon_send_saved fired.
        pub static SEND_SAVED: Cell<bool> = const { Cell::new(false) };
    }

    pub fn reset() {
        PARSE_UID.with(|c| c.set(None));
        EMITTED.with(|c| c.set(None));
        IS_GIF.with(|c| c.set(true));
        DATA_EMITTED.with(|c| c.set(None));
        DATA_COUNT.with(|c| c.set(0));
        STATE.with(|c| c.set(0));
        PROBE_TIMER.with(|c| c.set(0));
        SOURCE_REMOVED.with(|c| c.set(None));
        SEND_SAVED.with(|c| c.set(false));
    }
}

#[cfg(test)]
unsafe fn gtkhx_proto_parse_icon_change(_buf: *const u8, _len: usize, out_uid: *mut u16) -> bool {
    match test_env::PARSE_UID.with(|c| c.get()) {
        Some(uid) => {
            *out_uid = uid;
            true
        }
        None => false,
    }
}

#[cfg(test)]
unsafe fn gtkhx_session_get_default() -> *mut c_void {
    std::ptr::null_mut()
}

#[cfg(test)]
unsafe fn gtkhx_session_emit_gif_icon_changed(_self_: *mut c_void, _htlc: *mut c_void, uid: u16) {
    test_env::EMITTED.with(|c| c.set(Some(uid)));
}

#[cfg(test)]
unsafe fn gtkhx_proto_gif_icon_is_gif(_gif: *const u8, _len: usize) -> bool {
    test_env::IS_GIF.with(|c| c.get())
}

#[cfg(test)]
unsafe fn gtkhx_session_emit_gif_icon_data(
    _self_: *mut c_void,
    _htlc: *mut c_void,
    uid: u16,
    gif: *const c_void,
    len: u32,
) {
    test_env::DATA_EMITTED.with(|c| c.set(Some((uid, gif.is_null(), len))));
    test_env::DATA_COUNT.with(|c| c.set(c.get() + 1));
}

#[cfg(test)]
unsafe fn hx_conn_set_gif_icons_state(_h: *mut c_void, v: c_int) {
    test_env::STATE.with(|c| c.set(v));
}

#[cfg(test)]
unsafe fn hx_conn_gif_icons_probe_timer(_h: *const c_void) -> c_uint {
    test_env::PROBE_TIMER.with(|c| c.get())
}

#[cfg(test)]
unsafe fn hx_conn_set_gif_icons_probe_timer(_h: *mut c_void, v: c_uint) {
    test_env::PROBE_TIMER.with(|c| c.set(v));
}

#[cfg(test)]
unsafe fn g_source_remove(tag: c_uint) -> c_int {
    test_env::SOURCE_REMOVED.with(|c| c.set(Some(tag)));
    1
}

#[cfg(test)]
unsafe fn hx_icon_send_saved(_htlc: *mut c_void) {
    test_env::SEND_SAVED.with(|c| c.set(true));
}

#[cfg(test)]
mod tests;
