//! The Replies draft (capability bit 18): which target kinds a login said
//! the server keeps replies to, and what became of a request for lines by
//! id (`send::chat_history::quotes`), told on the `replies` signal. The
//! user never asked for those, so a refusal goes to the view alone, never
//! to the user. The target a line answers comes on its own entry, which
//! the session reads. A Notification (718), which GtkHx never asks for,
//! reaches the handlers where the session has replies, and none answers
//! it.

use std::cell::RefCell;
use std::collections::HashMap;
use std::ffi::c_void;

use gtkhx_core::conn::hx_conn_has_cap;
use gtkhx_core::replies::Event;
use hxproto::replies::Targets;
use hxproto::structured_chat::RATE_LIMITED;
use hxsession::HistoryEntry;

#[cfg(not(test))]
use gtkhx_core::session::{gtkhx_session_emit_replies, gtkhx_session_get_default};
#[cfg(test)]
use tests::{gtkhx_session_emit_replies, gtkhx_session_get_default};

thread_local! {
    /// What each connection's login said beside bit 18.
    static TARGETS: RefCell<HashMap<usize, Targets>> = RefCell::new(HashMap::new());
}

/// The login on `htlc` said `targets` beside bit 18, or nothing.
pub(crate) fn agreed(htlc: *mut c_void, targets: Option<Targets>) {
    TARGETS.with_borrow_mut(|t| match targets {
        Some(targets) => t.insert(htlc as usize, targets),
        None => t.remove(&(htlc as usize)),
    });
}

/// The target kinds the server keeps replies to, where `htlc`'s session
/// has replies: the bit, and the kinds the draft requires beside it.
///
/// # Safety
/// Main thread; `htlc` is a live connection.
pub unsafe fn targets(htlc: *mut c_void) -> Option<Targets> {
    if hx_conn_has_cap(htlc.cast(), hxsession::cap::REPLIES) == 0 {
        return None;
    }
    TARGETS.with_borrow(|t| t.get(&(htlc as usize)).copied())
}

/// Let go of what `htlc` agreed: a new login says again.
pub(crate) fn forget(htlc: *mut c_void) {
    agreed(htlc, None);
}

/// The answer to the request for chat `cid`'s lines `asked`.
///
/// # Safety
/// Main thread.
pub(crate) unsafe fn quoted(
    htlc: *mut c_void,
    cid: u32,
    asked: Vec<u64>,
    entries: &[HistoryEntry],
    has_more: bool,
) {
    let e = Event::Quotes {
        cid,
        asked,
        entries: entries.to_vec(),
        has_more,
    };
    gtkhx_session_emit_replies(gtkhx_session_get_default(), htlc, &e);
}

/// The request for chat `cid`'s lines `asked` was refused, with the
/// draft's `code` and the server's `reason`; one cut short, which is no
/// refusal, is told as one for the rate, to be asked again.
///
/// # Safety
/// Main thread.
pub(crate) unsafe fn refused(
    htlc: *mut c_void,
    cid: u32,
    asked: Vec<u64>,
    code: Option<u16>,
    reason: Option<&str>,
) {
    let slow = code == Some(RATE_LIMITED) || reason == Some(super::chat::CUT_SHORT);
    let e = Event::Refused { cid, asked, slow };
    gtkhx_session_emit_replies(gtkhx_session_get_default(), htlc, &e);
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::recv::chat::test_env::{self, Emitted};

    pub(super) unsafe fn gtkhx_session_get_default() -> *mut c_void {
        std::ptr::null_mut()
    }

    pub(super) unsafe fn gtkhx_session_emit_replies(_s: *mut c_void, _h: *mut c_void, e: &Event) {
        test_env::emit(match e {
            Event::Quotes {
                cid,
                asked,
                entries,
                has_more,
            } => Emitted::Quotes {
                cid: *cid,
                asked: asked.clone(),
                ids: entries.iter().map(|e| e.message_id).collect(),
                has_more: *has_more,
            },
            Event::Refused { cid, asked, slow } => Emitted::QuotesRefused {
                cid: *cid,
                asked: asked.clone(),
                slow: *slow,
            },
        });
    }
}
