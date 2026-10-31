//! The messaging extension's friend family (capability bit 6): what the
//! server pushes, and the replies to what `send::friends` asked, each
//! matched by its trans. hxnet hands both over as the session's own
//! `Unhandled` and `Reply`; what they say goes into the connection's
//! roster (`gtkhx_core::messaging`) and out on the `messaging` signal.
//!
//! Nothing here acts for a connection the server did not confirm bit 6 on,
//! so a server that never heard of the extension changes nothing.

use std::cell::RefCell;
use std::collections::HashMap;
use std::ffi::{c_char, c_void};
use std::time::Instant;

use gtkhx_core::conn::hx_conn_has_cap;
use gtkhx_core::messaging::requests::Page;
use gtkhx_core::messaging::{self as model, Delivery, Event, Friend, Guid, Im, Sender, Waiting};
use hxproto::messages::{tag, ServerHdr};
use hxproto::messaging::{
    parse_entry, parse_message_requests, parse_roster, AckType, MessageRequests, PresenceState,
    ReasonCode, RequestAction, TypingState, CAP_MESSAGING,
};
use hxproto::parse::Header;
use hxproto::wire::{Chunk, ChunkIter};

use super::chat::c_text;

#[cfg(not(test))]
use gtkhx_core::session::{gtkhx_session_emit_messaging, gtkhx_session_get_default};
#[cfg(test)]
use tests::{debug_log_str, gtkhx_session_emit_messaging, gtkhx_session_get_default};

#[cfg(not(test))]
extern "C" {
    /// Log a pre-formatted line under a debug category (`debug.c`).
    fn debug_log_str(cat: *const c_char, msg: *const c_char);
}

/// What a request in flight is answered into.
#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) enum Asked {
    Roster,
    /// Our presence, if it said, the status sent with it, if any, and
    /// whether we take message requests, if it said.
    Presence(Option<PresenceState>, Option<String>, Option<bool>),
    /// An instant message.
    Im(Guid),
    /// Find User or Get User Info: an entry, or nobody.
    Find,
    /// A page of the message requests' listing.
    Requests,
    /// An answer to a message request.
    Answer(RequestAction),
    /// Clear Message Requests.
    Clear,
    /// The rest, whose effect the server pushes as Roster Entries: only a
    /// refusal is news.
    Change,
}

thread_local! {
    /// The requests in flight, by connection and trans: their opcode, and
    /// what their reply goes into.
    static ASKED: RefCell<HashMap<(usize, u32), (u32, Asked)>> = RefCell::new(HashMap::new());
}

/// A request of `opcode` goes out on `trans`, its reply to go into `what`.
pub(crate) fn asked(htlc: *mut c_void, trans: u32, opcode: u32, what: Asked) {
    ASKED.with_borrow_mut(|a| a.insert((htlc as usize, trans), (opcode, what)));
}

/// Let go of what `htlc` holds and asked: a new connection numbers its
/// requests afresh, and starts from its own roster. A message never
/// answered is lost with it.
///
/// # Safety
/// Main thread.
pub(crate) unsafe fn forget(htlc: *mut c_void) {
    ASKED.with_borrow_mut(|a| a.retain(|(h, _), _| *h != htlc as usize));
    for (guid, to) in model::forget(htlc) {
        let delivery = Delivery::Failed;
        tell(htlc, &Event::Delivery { to, guid, delivery });
    }
}

unsafe fn tell(htlc: *mut c_void, e: &Event) {
    gtkhx_session_emit_messaging(gtkhx_session_get_default(), htlc, e);
}

unsafe fn log(line: &str) {
    debug_log_str(c"messaging".as_ptr(), c_text(line).as_ptr());
}

unsafe fn negotiated(htlc: *mut c_void) -> bool {
    hx_conn_has_cap(htlc.cast(), CAP_MESSAGING) != 0
}

/// The reply to the request on `trans`, whole.
///
/// # Safety
/// Main thread; `htlc` is a live connection.
pub(crate) unsafe fn reply(htlc: *mut c_void, trans: u32, frame: &[u8]) {
    let Some((opcode, what)) = ASKED.with_borrow_mut(|a| a.remove(&(htlc as usize, trans))) else {
        return;
    };
    let reason = reason_of(frame);
    if Header::parse(frame).is_some_and(|h| h.in_error()) {
        refused(htlc, trans, opcode, what, reason, frame);
        return;
    }
    match what {
        Asked::Roster => match parse_roster(frame, frame.len()) {
            Ok(r) => {
                let entries = r.entries().map(|e| Friend::from_entry(&e));
                model::with_roster_mut(htlc, |m| m.replace(entries));
                tell(htlc, &Event::Roster);
            }
            Err(e) => unreadable(htlc, trans, opcode, &format!("{e:?}")),
        },
        Asked::Presence(state, status, takes) => {
            model::with_roster_mut(htlc, |m| {
                if let Some(state) = state {
                    m.set_own_presence(state, status);
                }
                m.requests.takes = takes.or(m.requests.takes);
            });
            tell(htlc, &Event::Presence);
        }
        Asked::Im(guid) => delivered(htlc, &guid, None, Delivery::sent(reason)),
        Asked::Find => match parse_roster(frame, frame.len()) {
            Ok(r) => {
                let friend = r.entries().next().map(|e| Friend::from_entry(&e));
                tell(htlc, &Event::Found { trans, friend });
            }
            Err(e) => unreadable(htlc, trans, opcode, &format!("{e:?}")),
        },
        Asked::Requests => {
            match parse_message_requests(frame, frame.len()) {
                Ok(p) => {
                    let page = page(htlc, &p);
                    let now = Instant::now();
                    if model::with_roster_mut(htlc, |m| m.requests.page(page, now)) {
                        tell(htlc, &Event::Requests);
                    }
                }
                Err(e) => {
                    model::with_roster_mut(htlc, |m| m.requests.failed());
                    unreadable(htlc, trans, opcode, &format!("{e:?}"));
                }
            }
            crate::send::friends::pace(htlc, Instant::now());
        }
        // Approving and deleting change what waits, which the server says
        // with a Message Requests Changed; ending an approval does not.
        Asked::Answer(RequestAction::EndApproval) => crate::send::friends::requests(htlc),
        Asked::Clear => {
            let count = |t| chunk(frame, t).map_or(0, |c| c.as_uint());
            tell(
                htlc,
                &Event::Cleared {
                    trans,
                    requests: count(tag::REQUEST_COUNT),
                    friend_requests: count(tag::FRIEND_REQUEST_COUNT),
                },
            );
        }
        Asked::Answer(_) | Asked::Change => {}
    }
}

/// A page of the listing, its text in the connection's encoding.
unsafe fn page(htlc: *mut c_void, p: &MessageRequests<'_>) -> Page {
    let utf8 = hx_conn_has_cap(htlc.cast(), hxsession::cap::TEXT_ENCODING) != 0;
    let (mut senders, mut approvals) = (Vec::new(), Vec::new());
    for g in p.groups() {
        if g.is_approval() {
            approvals.push(g.login.to_vec());
            continue;
        }
        let messages = g.messages().map(|m| Waiting {
            guid: *m.guid,
            body: lf(&if utf8 {
                hxproto::text::to_utf8(m.body)
            } else {
                hxproto::text::mac_roman_to_utf8(m.body)
            }),
            sent_at: m.timestamp,
        });
        senders.push(Sender {
            login: g.login.to_vec(),
            messages: messages.collect(),
        });
    }
    Page {
        takes: p.takes_requests,
        dropped: p.dropped,
        cursor: p.cursor.map(<[u8]>::to_vec),
        senders,
        approvals,
    }
}

/// Line endings as GtkHx keeps them, LF.
fn lf(text: &str) -> String {
    text.replace("\r\n", "\n").replace('\r', "\n")
}

/// A reply that does not say what it must is no answer, and is said to be
/// none rather than taken for an empty one.
unsafe fn unreadable(htlc: *mut c_void, trans: u32, request: u32, why: &str) {
    log(&format!("the reply to {request} on trans {trans}: {why}"));
    let (reason, text) = (None, None);
    tell(
        htlc,
        &Event::Refused {
            trans,
            request,
            reason,
            text,
        },
    );
}

unsafe fn refused(
    htlc: *mut c_void,
    trans: u32,
    request: u32,
    what: Asked,
    reason: Option<ReasonCode>,
    frame: &[u8],
) {
    match what {
        // Nobody by that Login, or nobody the caller may see, is an answer.
        Asked::Find
            if matches!(
                reason,
                Some(ReasonCode::AccountNotFound | ReasonCode::NotMessagingEnabled)
            ) =>
        {
            tell(
                htlc,
                &Event::Found {
                    trans,
                    friend: None,
                },
            );
            return;
        }
        Asked::Im(guid) => delivered(htlc, &guid, None, Delivery::Failed),
        Asked::Requests => {
            model::with_roster_mut(htlc, |m| m.requests.failed());
            crate::send::friends::pace(htlc, Instant::now());
        }
        _ => {}
    }
    let text = hxproto::parse::parse_task_error(frame, frame.len(), 4096)
        .map(|t| hxproto::text::to_utf8(&t));
    tell(
        htlc,
        &Event::Refused {
            trans,
            request,
            reason,
            text,
        },
    );
}

/// `guid` moved on to `now`, where that moves it forward. A receipt names
/// who sends it (`by`), and only its recipient's counts; `None` is the
/// server answering the send.
unsafe fn delivered(htlc: *mut c_void, guid: &Guid, by: Option<&[u8]>, now: Delivery) {
    if let Some((to, delivery)) = model::with_roster_mut(htlc, |m| m.delivered(guid, by, now)) {
        tell(
            htlc,
            &Event::Delivery {
                to,
                guid: *guid,
                delivery,
            },
        );
    }
}

fn reason_of(frame: &[u8]) -> Option<ReasonCode> {
    let c = fields(frame).find(|c| c.tag == tag::REASON_CODE)?;
    ReasonCode::from_wire(u16::try_from(c.as_uint()).ok()?)
}

fn fields(frame: &[u8]) -> ChunkIter<'_> {
    ChunkIter::over_message(frame, frame.len())
}

fn chunk(frame: &[u8], t: u16) -> Option<Chunk<'_>> {
    fields(frame).find(|c| c.tag == t)
}

fn field(frame: &[u8], t: u16) -> Option<&[u8]> {
    chunk(frame, t).map(|c| c.data)
}

/// What the server sent of the extension unasked.
///
/// # Safety
/// Main thread; `htlc` is a live connection.
pub(crate) unsafe fn pushed(htlc: *mut c_void, opcode: u32, frame: &[u8]) {
    if !negotiated(htlc) {
        return;
    }
    let entry = || parse_entry(frame, frame.len()).map(|e| Friend::from_entry(&e));
    let hdr = |h: ServerHdr| h as u32;
    match opcode {
        o if o == hdr(ServerHdr::RosterEntry) => match entry() {
            Ok(f) => {
                let login = f.login.clone();
                model::with_roster_mut(htlc, |m| m.entry(f));
                tell(htlc, &Event::Changed { login });
            }
            Err(e) => log(&format!("roster entry: {e:?}")),
        },
        o if o == hdr(ServerHdr::FriendRequest) => match entry() {
            Ok(f) => {
                let login = f.login.clone();
                model::with_roster_mut(htlc, |m| m.request(f));
                tell(htlc, &Event::Request { login });
            }
            Err(e) => log(&format!("friend request: {e:?}")),
        },
        o if o == hdr(ServerHdr::PresenceChanged) => match entry() {
            Ok(f) => {
                if model::with_roster_mut(htlc, |m| m.presence(&f)) {
                    tell(htlc, &Event::Changed { login: f.login });
                }
            }
            Err(e) => log(&format!("presence change: {e:?}")),
        },
        o if o == hdr(ServerHdr::ImDeliver) => match im(frame) {
            Some(m) => {
                crate::send::friends::delivered(htlc, &m.guid, &m.from);
                tell(htlc, &Event::Im(m));
            }
            None => log("an instant message without its sender, id or text"),
        },
        o if o == hdr(ServerHdr::ImAck) => {
            let guid = field(frame, tag::MESSAGE_GUID).and_then(|g| Guid::try_from(g).ok());
            let ack = chunk(frame, tag::ACK_TYPE)
                .and_then(|a| u16::try_from(a.as_uint()).ok())
                .and_then(AckType::from_wire);
            let from = field(frame, tag::FRIEND_LOGIN);
            if let (Some(guid), Some(ack), Some(from)) = (guid, ack, from) {
                delivered(htlc, &guid, Some(from), Delivery::acked(ack));
            }
        }
        o if o == hdr(ServerHdr::ImTyping) => {
            let from = field(frame, tag::FRIEND_LOGIN).filter(|l| !l.is_empty());
            let state = chunk(frame, tag::TYPING_STATE).map(|c| c.as_uint());
            if let (Some(from), Some(state)) = (from, state) {
                let typing = state == u32::from(TypingState::Started.wire());
                tell(
                    htlc,
                    &Event::Typing {
                        from: from.to_vec(),
                        typing,
                    },
                );
            }
        }
        o if o == hdr(ServerHdr::MessageRequestsChanged) => {
            let count = chunk(frame, tag::REQUEST_COUNT).map(|c| c.as_uint());
            match count {
                Some(n) if model::with_roster(htlc, |m| m.requests.served) => {
                    model::with_roster_mut(htlc, |m| m.requests.changed(n));
                    tell(htlc, &Event::RequestsChanged);
                }
                Some(_) => {}
                None => log("message requests changed, without their count"),
            }
        }
        // Calls, transfers and buddy icons are not this client's yet.
        _ => {}
    }
}

/// An IM Deliver (811), where it carries what one must: its sender, its
/// id and its text.
fn im(frame: &[u8]) -> Option<Im> {
    let from = field(frame, tag::FRIEND_LOGIN).filter(|l| !l.is_empty())?;
    let guid = Guid::try_from(field(frame, tag::MESSAGE_GUID)?).ok()?;
    let body = hxproto::text::to_utf8(field(frame, tag::MESSAGE_BODY)?);
    let sent_at = field(frame, tag::MESSAGE_TIMESTAMP)
        .and_then(|t| <[u8; 8]>::try_from(t).ok())
        .map_or(0, u64::from_be_bytes);
    Some(Im {
        from: from.to_vec(),
        guid,
        body: lf(&body),
        sent_at,
    })
}

#[cfg(test)]
mod tests;
