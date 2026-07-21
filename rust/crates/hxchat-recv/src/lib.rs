//! Chat-domain receive handlers (ported from `rcv.c`).
//!
//! The server-notification handlers in `rcv.c` are thin glue: parse the frame
//! (still C — `proto_helpers` reads `htlc->in`), consult the per-chat model,
//! and emit a `GtkhxSession` signal. This crate owns the model-gate + emit half
//! so the decision (e.g. "drop an invite from an ignored user") is unit-testable
//! headlessly, instead of only against a live server. The C handler keeps the
//! parse + the model lookup and calls in here.

use std::os::raw::{c_char, c_int, c_void};

#[cfg(not(test))]
extern "C" {
    /// The singleton `GtkhxSession` GObject (gtkhx-session).
    fn gtkhx_session_get_default() -> *mut c_void;
    /// Fire `GtkhxSession::chat-invitation (htlc, cid, name)` (gtkhx-session).
    fn gtkhx_session_emit_chat_invitation(
        self_: *mut c_void,
        htlc: *mut c_void,
        cid: u32,
        name: *const c_char,
    );
    /// Whether `uid` is on the per-chat ignore list (hxmember-model).
    fn hx_member_model_get_ignore(model: *mut c_void, uid: u16) -> c_int;
}

/// `void hx_chat_invite_recv (htlc, member_model, cid, uid, name)` — the
/// private-chat-invitation receive path: drop the invite if the inviter is on
/// the ignore list, otherwise emit the `chat-invitation` signal (the sound
/// subscriber chimes off it). `member_model` is the public chat's member model
/// (`hx_chat_member_model(chat_with_cid(sess, 0))`); `name` is the inviter's
/// wire name as the parser produced it — `strip_ansi`'d and length-capped, but
/// NOT encoding-converted, so it's the server's raw bytes (often Mac Roman) and
/// is not guaranteed valid UTF-8. It's only forwarded to the signal here; any
/// Mac Roman → UTF-8 conversion happens later on the view side.
///
/// # Safety
/// `member_model` is a valid `HxMemberModel *`; `name` is a valid C string;
/// `htlc` is the connection handle (opaque, only forwarded to the signal).
#[no_mangle]
pub unsafe extern "C" fn hx_chat_invite_recv(
    htlc: *mut c_void,
    member_model: *mut c_void,
    cid: u32,
    uid: u16,
    name: *const c_char,
) {
    if hx_member_model_get_ignore(member_model, uid) != 0 {
        return;
    }
    gtkhx_session_emit_chat_invitation(gtkhx_session_get_default(), htlc, cid, name);
}

// ---- test doubles for the C environment ------------------------------------

#[cfg(test)]
pub(crate) mod test_env {
    use std::cell::Cell;

    thread_local! {
        /// Configurable return for the stubbed ignore lookup.
        pub static IGNORE: Cell<bool> = const { Cell::new(false) };
        /// Records the last emitted invitation as (cid, name-bytes), or None.
        pub static EMITTED: Cell<Option<(u32, Vec<u8>)>> = const { Cell::new(None) };
    }

    pub fn reset() {
        IGNORE.with(|c| c.set(false));
        EMITTED.with(|c| c.set(None));
    }
}

#[cfg(test)]
unsafe fn gtkhx_session_get_default() -> *mut c_void {
    std::ptr::null_mut()
}

#[cfg(test)]
unsafe fn gtkhx_session_emit_chat_invitation(
    _self_: *mut c_void,
    _htlc: *mut c_void,
    cid: u32,
    name: *const c_char,
) {
    let bytes = if name.is_null() {
        Vec::new()
    } else {
        std::ffi::CStr::from_ptr(name).to_bytes().to_vec()
    };
    test_env::EMITTED.with(|c| c.set(Some((cid, bytes))));
}

#[cfg(test)]
unsafe fn hx_member_model_get_ignore(_model: *mut c_void, _uid: u16) -> c_int {
    c_int::from(test_env::IGNORE.with(|c| c.get()))
}

#[cfg(test)]
mod tests;
