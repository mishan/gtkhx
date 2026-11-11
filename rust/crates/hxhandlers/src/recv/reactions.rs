//! The Reactions draft (capability bit 19): what the server pushes in
//! Reaction Changed (711), and the replies to the Send Reaction (710) and
//! Get Reactions (712) that `send::reactions` asked, each matched by its
//! trans. hxnet hands both over as the session's own `Unhandled` and
//! `Reply` where the session has reactions; what they say goes out on the
//! `reactions` signal, a target at a time. A Notification (718), which
//! GtkHx never asks for, is dropped.

use std::cell::RefCell;
use std::collections::HashMap;
use std::ffi::c_void;

use gtkhx_core::conn::hx_conn_has_cap;
use gtkhx_core::reactions::Event;
use hxproto::messages::tag;
use hxproto::reactions::{parse_changed, parse_got, Group, Key, Limits, CHANGED};
use hxproto::structured_chat::{RATE_LIMITED, TOO_MANY_TARGETS};
use hxproto::target::Target;
use hxproto::wire::ChunkIter;

#[cfg(not(test))]
use gtkhx_core::session::{gtkhx_session_emit_reactions, gtkhx_session_get_default};
#[cfg(test)]
use tests::{gtkhx_session_emit_reactions, gtkhx_session_get_default};

thread_local! {
    /// What each connection's login said beside bit 19.
    static LIMITS: RefCell<HashMap<usize, Limits>> = RefCell::new(HashMap::new());
    /// The requests in flight, by connection and trans.
    static ASKED: RefCell<HashMap<(usize, u32), Asked>> = RefCell::new(HashMap::new());
}

/// What a request in flight asked.
#[derive(Debug)]
pub(crate) enum Asked {
    /// Get Reactions, after these targets in order.
    Get(Vec<Target>),
    /// Send Reaction, `add`ing `key` on `target` or taking it back; `again`
    /// once it has been asked to slow down.
    Send {
        target: Target,
        key: Key,
        add: bool,
        again: bool,
    },
}

/// The login on `htlc` said `limits` beside bit 19, or nothing.
pub(crate) fn agreed(htlc: *mut c_void, limits: Option<Limits>) {
    LIMITS.with_borrow_mut(|l| match limits {
        Some(limits) => l.insert(htlc as usize, limits),
        None => l.remove(&(htlc as usize)),
    });
}

/// What the server takes reactions on, where `htlc`'s session has
/// reactions: the bit, and the target kinds the draft requires beside it.
///
/// # Safety
/// Main thread; `htlc` is a live connection.
pub unsafe fn limits(htlc: *mut c_void) -> Option<Limits> {
    if hx_conn_has_cap(htlc.cast(), hxsession::cap::REACTIONS) == 0 {
        return None;
    }
    LIMITS.with_borrow(|l| l.get(&(htlc as usize)).copied())
}

/// `what` goes out on `trans`.
pub(crate) fn asked(htlc: *mut c_void, trans: u32, what: Asked) {
    ASKED.with_borrow_mut(|a| a.insert((htlc as usize, trans), what));
}

/// Let go of what `htlc` agreed and asked: a new login says again.
pub(crate) fn forget(htlc: *mut c_void) {
    ASKED.with_borrow_mut(|a| a.retain(|(h, _), _| *h != htlc as usize));
    crate::send::reactions::forget(htlc);
    agreed(htlc, None);
}

unsafe fn tell(htlc: *mut c_void, e: Event) {
    gtkhx_session_emit_reactions(gtkhx_session_get_default(), htlc, &e);
}

/// A Reaction Changed: each line's summary, or its clearing, told as it
/// is read. An instant message's change is not shown yet.
///
/// # Safety
/// Main thread; `htlc` is a live connection.
pub(crate) unsafe fn pushed(htlc: *mut c_void, opcode: u32, frame: &[u8]) {
    if opcode != CHANGED || limits(htlc).is_none() {
        return;
    }
    for g in parse_changed(ChunkIter::over_message(frame, frame.len())) {
        match g {
            Group::Summary {
                target,
                summary,
                mine,
            } => tell(
                htlc,
                Event::Now {
                    target,
                    reactions: Some((summary, mine)),
                },
            ),
            Group::Cleared { target } => tell(
                htlc,
                Event::Now {
                    target,
                    reactions: None,
                },
            ),
            Group::Change { .. } => {}
        }
    }
}

/// The reply to the request on `trans`.
///
/// A Send Reaction's says the reaction was made, with the key's count
/// after, or why not; one asked to slow down is asked again once, a second
/// later.
///
/// A Get Reactions' says what each target asked after holds, in the order
/// asked, none for one it left out. One refused as too many targets for
/// its reply is asked again in halves; any other refusal changes nothing.
///
/// # Safety
/// Main thread; `htlc` is a live connection.
pub(crate) unsafe fn reply(htlc: *mut c_void, trans: u32, frame: &[u8]) {
    let Some(what) = ASKED.with_borrow_mut(|a| a.remove(&(htlc as usize, trans))) else {
        return;
    };
    let field = |tag| ChunkIter::over_message(frame, frame.len()).find(|c| c.tag == tag);
    let refused = hxproto::parse::Header::parse(frame).is_some_and(|h| h.in_error());
    let reason = field(tag::CHAT_REASON)
        .filter(|_| refused)
        .and_then(|c| u16::try_from(c.as_uint()).ok());
    match what {
        Asked::Get(targets) if refused => {
            if reason == Some(TOO_MANY_TARGETS) && targets.len() > 1 {
                let (a, b) = targets.split_at(targets.len() / 2);
                crate::send::reactions::get(htlc, a);
                crate::send::reactions::get(htlc, b);
            }
        }
        Asked::Get(targets) => {
            let mut got = parse_got(ChunkIter::over_message(frame, frame.len()));
            for target in targets {
                let reactions = got
                    .iter()
                    .position(|g| g.target == target)
                    .map(|i| got.remove(i))
                    .map(|g| (g.summary, g.mine));
                tell(htlc, Event::Now { target, reactions });
            }
        }
        Asked::Send {
            target,
            key,
            add,
            again: false,
        } if reason == Some(RATE_LIMITED) => {
            crate::send::reactions::slow(htlc, std::time::Instant::now());
            crate::send::reactions::ask(htlc, target, key, add, true);
        }
        Asked::Send { target, key, .. } if refused => {
            let error =
                field(tag::TASK_ERROR).map(|c| String::from_utf8_lossy(c.data).into_owned());
            tell(
                htlc,
                Event::Refused {
                    target,
                    key,
                    reason,
                    error,
                },
            );
        }
        Asked::Send {
            target, key, add, ..
        } => {
            let count = field(tag::REACTION_COUNT).map(|c| c.as_uint());
            tell(
                htlc,
                Event::Acted {
                    target,
                    key,
                    add,
                    count,
                },
            );
        }
    }
}

#[cfg(test)]
mod tests;
