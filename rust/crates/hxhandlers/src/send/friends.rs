//! The messaging extension's friend family (capability bit 6), as the view
//! asks for it: the roster, friend requests and their answers, blocks, our
//! presence, instant messages with their receipts and typing, finding an
//! account and a friend's alias. Each goes only where the server confirmed
//! bit 6, and says nothing otherwise: `None`, nothing sent.
//!
//! A Login is bytes as the wire carries it, from an entry or from
//! [`login`]; text is UTF-8, sent in the connection's encoding. What a
//! reply says arrives on the `messaging` signal (`recv::friends`), a
//! request's trans naming its answer.

use std::os::raw::{c_int, c_void};

use gtkhx_core::conn::hx_conn_has_cap;
use gtkhx_core::messaging::{with_roster_mut, Guid};
use hxproto::messaging::{AckType, PresenceState, CAP_MESSAGING};
use hxrequest::messaging as req;
use hxrequest::Request;
use hxsession::cap;

use crate::recv::friends::{asked, Asked};

#[cfg(not(test))]
use hxtask::send::{hlwrite_chunks, next_trans};
#[cfg(test)]
use tests::{hlwrite_chunks, next_trans};

unsafe fn utf8(htlc: *mut c_void) -> bool {
    hx_conn_has_cap(htlc.cast(), cap::TEXT_ENCODING) != 0
}

/// Send `r` on a connection that negotiated the friend family, its reply
/// going into `what` where there is one, and unheard where there is none.
/// The trans it went on.
unsafe fn send(htlc: *mut c_void, r: Option<Request>, what: Option<Asked>) -> Option<u32> {
    if hx_conn_has_cap(htlc.cast(), CAP_MESSAGING) == 0 {
        return None;
    }
    let r = r?;
    let trans = next_trans(htlc.cast());
    if let Some(what) = what {
        asked(htlc, trans, r.opcode, what);
    }
    r.with_hx_chunks(|c| hlwrite_chunks(htlc.cast(), r.opcode, 0, c.as_ptr(), c.len() as c_int));
    Some(trans)
}

/// A Login the user typed, as this connection's wire carries it.
///
/// # Safety
/// `htlc` is a live connection; main thread, as for every sender here.
pub unsafe fn login(htlc: *mut c_void, typed: &str) -> Vec<u8> {
    req::login(typed.as_bytes(), utf8(htlc))
}

/// Read the roster again, which hands over what waited for us too.
///
/// # Safety
/// See [`login`].
pub unsafe fn roster(htlc: *mut c_void) -> Option<u32> {
    send(htlc, Some(req::roster()), Some(Asked::Roster))
}

/// Ask `login` to be a friend, with a `note`.
///
/// # Safety
/// See [`login`].
pub unsafe fn add(htlc: *mut c_void, login: &[u8], note: Option<&str>) -> Option<u32> {
    let r = req::add_friend(login, note.map(str::as_bytes), utf8(htlc));
    send(htlc, r, Some(Asked::Change))
}

/// End a friendship, or withdraw our request.
///
/// # Safety
/// See [`login`].
pub unsafe fn remove(htlc: *mut c_void, login: &[u8]) -> Option<u32> {
    send(htlc, req::remove_friend(login), Some(Asked::Change))
}

/// Accept `login`'s request, or decline it.
///
/// # Safety
/// See [`login`].
pub unsafe fn respond(htlc: *mut c_void, login: &[u8], accept: bool) -> Option<u32> {
    send(htlc, req::respond(login, accept), Some(Asked::Change))
}

/// Block `login`, which ends any friendship with them.
///
/// # Safety
/// See [`login`].
pub unsafe fn block(htlc: *mut c_void, login: &[u8]) -> Option<u32> {
    send(htlc, req::block(login), Some(Asked::Change))
}

/// # Safety
/// See [`login`].
pub unsafe fn unblock(htlc: *mut c_void, login: &[u8]) -> Option<u32> {
    send(htlc, req::unblock(login), Some(Asked::Change))
}

/// Our presence, and our status where `status` is given; an empty one
/// clears it.
///
/// # Safety
/// See [`login`].
pub unsafe fn set_presence(
    htlc: *mut c_void,
    state: PresenceState,
    status: Option<&str>,
) -> Option<u32> {
    let r = req::set_presence(state, status.map(str::as_bytes), utf8(htlc));
    let what = Asked::Presence(state, status.map(str::to_owned));
    send(htlc, r, Some(what))
}

/// An instant message to `to`, and the id its receipts will name.
///
/// # Safety
/// See [`login`].
pub unsafe fn im(htlc: *mut c_void, to: &[u8], body: &str) -> Option<Guid> {
    let guid = new_guid()?;
    let r = req::im_send(to, &guid, body.as_bytes(), utf8(htlc));
    send(htlc, r, Some(Asked::Im(guid)))?;
    with_roster_mut(htlc, |m| m.sending(guid, to));
    Some(guid)
}

/// `sender`'s message `guid` has been seen.
///
/// # Safety
/// See [`login`].
pub unsafe fn read(htlc: *mut c_void, guid: &Guid, sender: &[u8]) -> Option<u32> {
    let r = req::im_ack(guid, AckType::Read, sender);
    send(htlc, r, Some(Asked::Change))
}

/// `sender`'s message `guid` arrived. Sent unasked by the user, so its
/// refusal is nobody's news.
pub(crate) unsafe fn delivered(htlc: *mut c_void, guid: &Guid, sender: &[u8]) -> Option<u32> {
    send(htlc, req::im_ack(guid, AckType::Delivered, sender), None)
}

/// We started, or stopped, typing to `login`. Never answered.
///
/// # Safety
/// See [`login`].
pub unsafe fn typing(htlc: *mut c_void, login: &[u8], typing: bool) -> Option<u32> {
    send(htlc, req::im_typing(login, typing), None)
}

/// The account `login` names exactly.
///
/// # Safety
/// See [`login`].
pub unsafe fn find(htlc: *mut c_void, login: &[u8]) -> Option<u32> {
    send(htlc, req::find_user(login), Some(Asked::Find))
}

/// What `login` shows of itself.
///
/// # Safety
/// See [`login`].
pub unsafe fn profile(htlc: *mut c_void, login: &[u8]) -> Option<u32> {
    send(htlc, req::profile(login), Some(Asked::Find))
}

/// Our alias for `login`; an empty one clears it.
///
/// # Safety
/// See [`login`].
pub unsafe fn set_nickname(htlc: *mut c_void, login: &[u8], nickname: &str) -> Option<u32> {
    let r = req::set_nickname(login, nickname.as_bytes(), utf8(htlc));
    send(htlc, r, Some(Asked::Change))
}

/// A random message id, a version 4 UUID as other clients make them.
fn new_guid() -> Option<Guid> {
    let mut g = [0u8; 16];
    getrandom::fill(&mut g).ok()?;
    g[6] = (g[6] & 0x0f) | 0x40;
    g[8] = (g[8] & 0x3f) | 0x80;
    Some(g)
}

#[cfg(test)]
pub(crate) mod tests;
