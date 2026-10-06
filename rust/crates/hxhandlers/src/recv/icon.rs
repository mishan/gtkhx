//! GIF icons: what the session made of the replies to the probe, a user's
//! icon and our own set, and the server's `ICON_CHANGE` notice.
//!
//! The extension has no capability bit and no version tie, so whether a
//! server has it is found by the probe, the icon list asked for at login: a
//! listing says it does, and a refusal, or no answer by the watchdog
//! (`send::icon`), that it does not. Neither the probe's refusal nor that of
//! the saved avatar sent once the server proves capable is the user's to
//! hear of: they never asked. A refusal of anything else they did ask for
//! goes to `request-failed`. The `ICON_CHANGE` broadcast still arrives
//! whole; its parse is a bytes-in Rust parser, and the C side a one-line
//! forwarder.

use std::cell::RefCell;
use std::collections::HashMap;
use std::os::raw::{c_char, c_int, c_uint, c_void};

use hxsession::Icon;

#[cfg(not(test))]
use gtkhx_core::session::{
    gtkhx_session_emit_gif_icon_changed, gtkhx_session_emit_gif_icon_data,
    gtkhx_session_get_default,
};
#[cfg(not(test))]
use gtkhx_proto_ffi::ffi::{gtkhx_proto_gif_icon_is_gif, gtkhx_proto_parse_icon_change};

/// GIF-icons negotiation tri-state (mirror of the C `enum` in `gif_icons.h`).
pub(crate) const GIF_ICONS_UNKNOWN: c_int = 0;
const GIF_ICONS_SUPPORTED: c_int = 1;
pub(crate) const GIF_ICONS_UNSUPPORTED: c_int = 2;

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
    /// Log a pre-formatted line under a debug category (`debug.c`).
    fn debug_log_str(cat: *const c_char, msg: *const c_char);
}

/// The icon requests whose refusal the user does not hear of.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum Asked {
    /// The login's probe: a refusal says the server has no GIF icons.
    Probe,
    /// The saved avatar, sent without the user asking.
    Saved,
}

thread_local! {
    /// The requests in flight, by connection and trans.
    static ASKED: RefCell<HashMap<(usize, u32), Asked>> = RefCell::new(HashMap::new());
}

pub(crate) fn asked(htlc: *mut c_void, trans: u32, what: Asked) {
    ASKED.with(|a| a.borrow_mut().insert((htlc as usize, trans), what));
}

fn answered(htlc: *mut c_void, trans: u32) -> Option<Asked> {
    ASKED.with(|a| a.borrow_mut().remove(&(htlc as usize, trans)))
}

/// Let go of what `htlc` asked for.
pub(crate) fn forget(htlc: *mut c_void) {
    ASKED.with(|a| a.borrow_mut().retain(|(h, _), _| *h != htlc as usize));
}

/// Disarm the GIF-icons probe watchdog if armed (the reply beat the timeout).
unsafe fn disarm_probe_timer(htlc: *mut c_void) {
    let tag = hx_conn_gif_icons_probe_timer(htlc);
    if tag != 0 {
        g_source_remove(tag);
        hx_conn_set_gif_icons_probe_timer(htlc, 0);
    }
}

/// Every user's icon: the server has GIF icons, so ours goes up, and each
/// listed one is published. A late answer to a probe the watchdog gave up
/// on counts the same: the server was slow, not without them.
///
/// # Safety
/// Main thread; `htlc` is a live connection.
pub(crate) unsafe fn listed(htlc: *mut c_void, trans: u32, icons: &[Icon]) {
    answered(htlc, trans);
    hx_conn_set_gif_icons_state(htlc, GIF_ICONS_SUPPORTED);
    disarm_probe_timer(htlc);
    hx_icon_send_saved(htlc);
    // A uid is a u16, so a well-formed list holds at most 65536 entries; clamp
    // the walk so a hostile/duplicated reply can't drive an unbounded emit storm.
    for i in icons.iter().take(u16::MAX as usize + 1) {
        hx_icon_data_recv(htlc, i.uid, i.gif.as_ptr(), i.gif.len() as u32);
    }
}

/// One user's icon. An answer at all says the server has GIF icons; an
/// empty icon clears the user's.
///
/// # Safety
/// Main thread; `htlc` is a live connection.
pub(crate) unsafe fn icon(htlc: *mut c_void, icon: &Icon) {
    hx_conn_set_gif_icons_state(htlc, GIF_ICONS_SUPPORTED);
    hx_icon_data_recv(htlc, icon.uid, icon.gif.as_ptr(), icon.gif.len() as u32);
}

/// A request on `trans` was refused, or its reply cut short. Whether the
/// user is to hear of it: not of the probe, which marks the server as
/// without GIF icons, nor of the saved avatar, which goes to the `icon`
/// debug category; nothing sends it again on this connection.
///
/// # Safety
/// Main thread; `htlc` is a live connection.
pub(crate) unsafe fn failed(htlc: *mut c_void, trans: u32, reason: Option<&str>) -> bool {
    match answered(htlc, trans) {
        Some(Asked::Probe) => {
            hx_conn_set_gif_icons_state(htlc, GIF_ICONS_UNSUPPORTED);
            disarm_probe_timer(htlc);
            true
        }
        Some(Asked::Saved) => {
            let line = match reason.filter(|r| !r.is_empty()) {
                Some(r) => format!(
                    "server refused the saved avatar: {r}; not re-sending on this connection"
                ),
                None => {
                    "server refused the saved avatar; not re-sending on this connection".to_owned()
                }
            };
            if let Ok(c) = std::ffi::CString::new(line.replace('\0', "")) {
                debug_log_str(c"icon".as_ptr(), c.as_ptr());
            }
            true
        }
        None => false,
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
        /// Every line logged through debug_log_str, as (category, message).
        pub static DEBUG_LINES: std::cell::RefCell<Vec<(String, String)>> =
            const { std::cell::RefCell::new(Vec::new()) };
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
        DEBUG_LINES.with(|c| c.borrow_mut().clear());
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
unsafe fn debug_log_str(cat: *const c_char, msg: *const c_char) {
    let s = |p| std::ffi::CStr::from_ptr(p).to_string_lossy().into_owned();
    test_env::DEBUG_LINES.with(|c| c.borrow_mut().push((s(cat), s(msg))));
}

#[cfg(test)]
mod tests;
