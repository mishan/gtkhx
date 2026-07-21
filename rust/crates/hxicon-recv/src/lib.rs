//! GIF-icon receive path (ported from `rcv.c`).
//!
//! The GIF-icons extension broadcasts an `ICON_CHANGE` frame (uid only) when a
//! user changes their avatar. This crate owns that handler end to end: parse the
//! uid out of the frame body (via `hotline-proto`) and emit the
//! `gif-icon-changed` signal so the user list refreshes the avatar. Unlike the
//! chat-invite handler (whose parse stayed C because it reads `htlc->in` through
//! a struct), the icon-change parse is already a bytes-in Rust parser, so the
//! whole handler moves here and the C side is a one-line forwarder.

use std::os::raw::c_void;

#[cfg(not(test))]
extern "C" {
    /// Parse an `ICON_CHANGE` broadcast body: writes `*out_uid` and returns
    /// true when a uid is present (hotline-proto).
    fn gtkhx_proto_parse_icon_change(buf: *const u8, len: usize, out_uid: *mut u16) -> bool;
    /// The singleton `GtkhxSession` GObject (gtkhx-session).
    fn gtkhx_session_get_default() -> *mut c_void;
    /// Fire `GtkhxSession::gif-icon-changed (htlc, uid)` (gtkhx-session).
    fn gtkhx_session_emit_gif_icon_changed(self_: *mut c_void, htlc: *mut c_void, uid: u16);
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
    }

    pub fn reset() {
        PARSE_UID.with(|c| c.set(None));
        EMITTED.with(|c| c.set(None));
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
mod tests;
