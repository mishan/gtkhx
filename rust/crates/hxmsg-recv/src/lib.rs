//! Private-message receive path (ported from `rcv.c`).
//!
//! `hx_rcv_msg` handles both the private `MSG` and the server-wide
//! `MSG_BROADCAST` opcode: it drops anything from an ignored sender, then either
//! emits the boxed `msg` signal (a private message) or hands off to
//! `broadcastmsg` (a broadcast). This crate owns the shared ignore-gate + the
//! private-message emit — the testable decisions — while the C handler keeps the
//! wire parse, the self-PM display-name resolution, the boxed `HxMsgEvent`
//! lifetime, and the broadcast rendering (`broadcastmsg`, a view function).
//!
//! The C side classifies the frame (broadcast vs private) and, for a private
//! message, builds the event before calling in; [`hx_msg_recv`] returns which
//! branch to take so C can run `broadcastmsg` itself and preserve the
//! ignored-message early-out.

use std::os::raw::{c_int, c_void};

#[cfg(not(test))]
use gtkhx_session::{gtkhx_session_emit_msg, gtkhx_session_get_default};
#[cfg(not(test))]
use hxmodel::chat_members::hx_member_model_get_ignore;

/// Outcome of [`hx_msg_recv`], telling the C handler what happened / what to do.
/// The sender was on the ignore list — nothing emitted, nothing broadcast, and
/// the C side returns early (no `last_msg_nick` update).
pub const HX_MSG_DROPPED: c_int = 0;
/// A private message: the boxed `msg` signal was emitted.
pub const HX_MSG_EMITTED: c_int = 1;
/// A broadcast (or a bare server note): the C side runs `broadcastmsg`.
pub const HX_MSG_BROADCAST: c_int = 2;

/// `int hx_msg_recv (member_model, uid, is_pm, event)` — the MSG / MSG_BROADCAST
/// ignore-gate + private-message emit. Drops the message when `uid` is ignored
/// ([`HX_MSG_DROPPED`]); otherwise emits the boxed `msg` signal for a private
/// message ([`HX_MSG_EMITTED`]) or reports [`HX_MSG_BROADCAST`] so the C side
/// renders it via `broadcastmsg`.
///
/// `is_pm` is the C-side classification (`!is_broadcast && uid > 0`). `event` is
/// the boxed `HxMsgEvent*` the C side built for the private-message branch (NULL
/// on the broadcast branch); the C side keeps its lifetime and frees it after
/// this returns.
///
/// # Safety
/// `member_model` is a valid `HxMemberModel *`; `event` is a valid boxed
/// `HxMsgEvent *` when `is_pm` is set (else unused).
#[no_mangle]
pub unsafe extern "C" fn hx_msg_recv(
    member_model: *mut c_void,
    uid: u16,
    is_pm: c_int,
    event: *mut c_void,
) -> c_int {
    // The ignore list is keyed on uid; uid 0 (a server/system note) is never in
    // it, so this also lets bare broadcasts through — same as the C original.
    if hx_member_model_get_ignore(member_model, uid) != 0 {
        return HX_MSG_DROPPED;
    }
    if is_pm != 0 {
        // Contract: the C side always builds the boxed HxMsgEvent for the
        // private-message branch. A NULL here is a caller bug — emitting it would
        // set a NULL boxed value and crash a downstream `msg` handler — so flag
        // it loudly in dev and drop it safely in release rather than propagate.
        debug_assert!(!event.is_null(), "hx_msg_recv: is_pm set but event is NULL");
        if event.is_null() {
            return HX_MSG_DROPPED;
        }
        gtkhx_session_emit_msg(gtkhx_session_get_default(), event);
        return HX_MSG_EMITTED;
    }
    HX_MSG_BROADCAST
}

// ---- test doubles for the C environment ------------------------------------

#[cfg(test)]
pub(crate) mod test_env {
    use std::cell::Cell;

    thread_local! {
        /// Configurable return for the stubbed ignore lookup.
        pub static IGNORE: Cell<bool> = const { Cell::new(false) };
        /// Records the boxed-event pointer of the last emitted `msg`, or None.
        pub static EMITTED: Cell<Option<*mut std::os::raw::c_void>> = const { Cell::new(None) };
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
unsafe fn gtkhx_session_emit_msg(_self_: *mut c_void, event: *mut c_void) {
    test_env::EMITTED.with(|c| c.set(Some(event)));
}

#[cfg(test)]
unsafe fn hx_member_model_get_ignore(_model: *mut c_void, _uid: u16) -> c_int {
    c_int::from(test_env::IGNORE.with(|c| c.get()))
}

#[cfg(test)]
mod tests;
