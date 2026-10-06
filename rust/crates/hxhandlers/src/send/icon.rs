//! GIF icons: the probe that finds whether a server has them, a user's icon,
//! and ours set or cleared. The requests are `hxrequest::icon`'s; each reply
//! is expected by the session (`recv::icon`).

use std::os::raw::{c_char, c_int, c_uint, c_void};

use hxrequest::{icon, Request};
use hxsession::Expect;

use crate::recv::icon::{asked, Asked, GIF_ICONS_UNKNOWN, GIF_ICONS_UNSUPPORTED};

/// How long the probe waits for an answer: a server without GIF icons may
/// drop the request without one.
const PROBE_TIMEOUT_S: c_uint = 2;

#[cfg(not(test))]
use hxtask::send::hlwrite_chunks;

#[cfg(not(test))]
extern "C" {
    fn hx_conn_gif_icons_state(h: *const c_void) -> c_int;
    fn hx_conn_set_gif_icons_state(h: *mut c_void, v: c_int);
    fn hx_conn_gif_icons_probe_timer(h: *const c_void) -> c_uint;
    fn hx_conn_set_gif_icons_probe_timer(h: *mut c_void, v: c_uint);
    fn debug_log_str(cat: *const c_char, msg: *const c_char);
}

#[cfg(test)]
use tests::{
    debug_log_str, hlwrite_chunks, hx_conn_gif_icons_probe_timer, hx_conn_gif_icons_state,
    hx_conn_set_gif_icons_probe_timer, hx_conn_set_gif_icons_state,
};

unsafe fn log(line: &str) {
    if let Ok(c) = std::ffi::CString::new(line) {
        debug_log_str(c"icon".as_ptr(), c.as_ptr());
    }
}

/// Send `req`, its reply expected as `what`; the trans it went out on.
unsafe fn send(htlc: *mut c_void, req: &Request, what: Expect) -> u32 {
    let trans = super::expect_next(htlc, what);
    req.with_hx_chunks(|chunks| {
        hlwrite_chunks(
            htlc.cast(),
            req.opcode,
            0,
            chunks.as_ptr(),
            chunks.len() as c_int,
        )
    });
    trans
}

unsafe extern "C" fn probe_timeout(htlc: *mut c_void) -> glib::ffi::gboolean {
    hx_conn_set_gif_icons_probe_timer(htlc, 0);
    if hx_conn_gif_icons_state(htlc) == GIF_ICONS_UNKNOWN {
        hx_conn_set_gif_icons_state(htlc, GIF_ICONS_UNSUPPORTED);
        log("GIF-icons probe timed out; server appears not to support the extension");
    }
    glib::ffi::GFALSE
}

/// `void hx_icon_probe (struct htlc_conn *htlc)` — ask for every user's
/// icon, which finds whether the server has GIF icons at all. The watchdog
/// settles it as no once the answer is overdue; the connection's close
/// disarms it.
///
/// # Safety
/// `htlc` is NULL or a live connection; main thread.
#[no_mangle]
pub unsafe extern "C" fn hx_icon_probe(htlc: *mut c_void) {
    if htlc.is_null() {
        return;
    }
    hx_conn_set_gif_icons_state(htlc, GIF_ICONS_UNKNOWN);
    let old = hx_conn_gif_icons_probe_timer(htlc);
    if old != 0 {
        glib::ffi::g_source_remove(old);
    }
    let trans = send(htlc, &icon::list(), Expect::IconList);
    asked(htlc, trans, Asked::Probe);
    // GLib calls back with the pointer it was given, which the connection's
    // close disarms before the connection goes.
    let tag = glib::ffi::g_timeout_add_seconds(PROBE_TIMEOUT_S, Some(probe_timeout), htlc);
    hx_conn_set_gif_icons_probe_timer(htlc, tag);
    log("GIF-icons probe sent (ICON_GETLIST), watchdog armed");
}

/// `void hx_icon_get (struct htlc_conn *htlc, guint16 uid)` — `uid`'s icon.
///
/// # Safety
/// `htlc` is NULL or a live connection; main thread.
#[no_mangle]
pub unsafe extern "C" fn hx_icon_get(htlc: *mut c_void, uid: u16) {
    if htlc.is_null() {
        return;
    }
    if let Some(req) = icon::get(uid) {
        send(htlc, &req, Expect::Icon);
    }
}

/// Our icon becomes `gif`, or none when it is empty; the trans, unless it
/// was not sent. A non-empty one must be a GIF: the server takes nothing
/// else, so it is refused here rather than there.
unsafe fn set(htlc: *mut c_void, gif: *const u8, len: usize) -> Option<u32> {
    if htlc.is_null() {
        return None;
    }
    let gif = if len == 0 || gif.is_null() {
        &[][..]
    } else {
        std::slice::from_raw_parts(gif, len)
    };
    if !gif.is_empty() && !hxproto::gif_icons::is_gif(gif) {
        log(&format!(
            "refusing ICON_SET: payload is not a GIF ({} bytes)",
            gif.len()
        ));
        return None;
    }
    let Some(req) = icon::set(gif) else {
        log(&format!(
            "ICON_SET not sent: {} bytes is over the 64 KiB wire limit",
            gif.len()
        ));
        return None;
    };
    Some(send(htlc, &req, Expect::IconSet))
}

/// `void hx_icon_set (struct htlc_conn *htlc, const guint8 *gif, gsize len)`
/// — our icon, as the user picked it; a refusal reaches them.
///
/// # Safety
/// `htlc` is NULL or a live connection; `gif` points at `len` bytes when
/// `len` is not 0. Main thread.
#[no_mangle]
pub unsafe extern "C" fn hx_icon_set(htlc: *mut c_void, gif: *const u8, len: usize) {
    set(htlc, gif, len);
}

/// `void hx_icon_clear (struct htlc_conn *htlc)` — no icon.
///
/// # Safety
/// As [`hx_icon_set`].
#[no_mangle]
pub unsafe extern "C" fn hx_icon_clear(htlc: *mut c_void) {
    set(htlc, std::ptr::null(), 0);
}

/// `void hx_icon_set_saved (struct htlc_conn *htlc, const guint8 *gif,
/// gsize len)` — the avatar the user saved, sent without their asking once
/// a server proves capable; a refusal goes to the debug log, not to them.
///
/// # Safety
/// As [`hx_icon_set`].
#[no_mangle]
pub unsafe extern "C" fn hx_icon_set_saved(htlc: *mut c_void, gif: *const u8, len: usize) {
    if let Some(trans) = set(htlc, gif, len) {
        asked(htlc, trans, Asked::Saved);
    }
}

#[cfg(test)]
mod tests;
