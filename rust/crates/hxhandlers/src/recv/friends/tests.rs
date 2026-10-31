//! What the friend family's pushes and replies become: the roster a real
//! connection holds and the events the `messaging` signal would carry,
//! through a recording double of its emit. Frames are built as a server
//! builds them, with hxproto's own encoder.

use std::cell::RefCell;

use gtkhx_core::messaging::with_roster;
use hxproto::messaging::{PresenceState as P, RosterEntry, RosterState as S};
use hxproto::wire::Encoder;

use super::*;
use crate::send::friends::tests::{sent, Conn};

thread_local! {
    static TOLD: RefCell<Vec<Event>> = const { RefCell::new(Vec::new()) };
}

pub(super) unsafe fn gtkhx_session_emit_messaging(_s: *mut c_void, _htlc: *mut c_void, e: &Event) {
    TOLD.with(|t| t.borrow_mut().push(e.clone()));
}

pub(super) unsafe fn gtkhx_session_get_default() -> *mut c_void {
    std::ptr::null_mut()
}

pub(super) unsafe fn debug_log_str(_cat: *const c_char, _msg: *const c_char) {}

fn told() -> Vec<Event> {
    TOLD.with(|t| t.take())
}

const TASK: u32 = 0x0001_0000;

/// A whole transaction: `opcode` on `trans`, refused where `error`.
fn frame(opcode: u32, trans: u32, error: bool, body: impl FnOnce(&mut Encoder) -> u16) -> Vec<u8> {
    let mut fields = Encoder::new();
    let n = body(&mut fields);
    let fields = fields.into_vec();
    let mut f = Vec::new();
    f.extend_from_slice(&opcode.to_be_bytes());
    f.extend_from_slice(&trans.to_be_bytes());
    f.extend_from_slice(&u32::from(error).to_be_bytes());
    let len = (fields.len() + 2) as u32;
    f.extend_from_slice(&len.to_be_bytes());
    f.extend_from_slice(&len.to_be_bytes());
    f.extend_from_slice(&n.to_be_bytes());
    f.extend_from_slice(&fields);
    f
}

fn fields<'a>(list: &'a [(u16, &'a [u8])]) -> impl FnOnce(&mut Encoder) -> u16 + 'a {
    move |e| {
        for (t, d) in list {
            e.put_chunk(*t, d);
        }
        list.len() as u16
    }
}

fn entries<'a>(list: &'a [RosterEntry<'a>]) -> impl FnOnce(&mut Encoder) -> u16 + 'a {
    move |e| list.iter().map(|r| r.encode(e).unwrap()).sum()
}

fn entry<'a>(login: &'a [u8], state: Option<S>, presence: Option<P>) -> RosterEntry<'a> {
    RosterEntry {
        login,
        state: state.map(S::wire),
        presence: presence.map(P::wire),
        ..RosterEntry::default()
    }
}

fn messenger() -> Conn {
    sent();
    told();
    Conn::with_caps(CAP_MESSAGING)
}

fn login(e: &Event) -> &[u8] {
    match e {
        Event::Changed { login } | Event::Request { login } => login,
        other => panic!("{other:?}"),
    }
}

#[test]
fn a_server_that_never_confirmed_bit_6_changes_nothing() {
    told();
    let c = Conn::with_caps(0);
    let f = frame(
        801,
        0,
        false,
        entries(&[entry(b"bob", Some(S::Accepted), None)]),
    );
    unsafe { pushed(c.0, 801, &f) };
    assert_eq!(told(), []);
    assert!(with_roster(c.0, |r| r.get(b"bob").is_none()));
}

#[test]
fn the_roster_is_read_and_kept_up_by_what_the_server_pushes() {
    let c = messenger();
    let trans = unsafe { crate::send::friends::roster(c.0).unwrap() };
    let listed = [
        entry(b"alice", Some(S::Accepted), Some(P::Away)),
        entry(b"bob", Some(S::PendingOut), None),
        entry(b"carol", Some(S::Blocked), None),
    ];
    unsafe { reply(c.0, trans, &frame(TASK, trans, false, entries(&listed))) };
    assert_eq!(told(), [Event::Roster]);
    let state = |l: &[u8]| with_roster(c.0, |r| r.get(l).map(|f| (f.state, f.presence)));
    assert_eq!(state(b"alice"), Some((Some(S::Accepted), P::Away)));

    let note = RosterEntry {
        note: Some(b"hi, it's dave"),
        ..entry(b"dave", None, None)
    };
    let mut status = entry(b"alice", None, Some(P::Busy));
    status.status_text = Some(b"meeting");
    let pushes = [
        (801, entry(b"bob", Some(S::Accepted), Some(P::Online))),
        (801, entry(b"carol", Some(S::Removed), None)),
        (804, note),
        (809, status),
        // No friend of ours: nothing changed.
        (809, entry(b"stranger", None, Some(P::Online))),
    ];
    for (opcode, e) in pushes {
        unsafe { pushed(c.0, opcode, &frame(opcode, 0, false, entries(&[e]))) };
    }
    let events = told();
    let logins: Vec<_> = events.iter().map(login).collect();
    assert_eq!(logins, [&b"bob"[..], b"carol", b"dave", b"alice"]);
    assert!(matches!(events[2], Event::Request { .. }));
    assert_eq!(state(b"bob"), Some((Some(S::Accepted), P::Online)));
    assert_eq!(state(b"carol"), None);
    with_roster(c.0, |r| {
        let dave = r.get(b"dave").unwrap();
        assert_eq!(
            (dave.state, dave.note.as_deref()),
            (Some(S::PendingIn), Some("hi, it's dave"))
        );
        let alice = r.get(b"alice").unwrap();
        assert_eq!(
            (alice.state, alice.presence, alice.status.as_deref()),
            (Some(S::Accepted), P::Busy, Some("meeting"))
        );
    });
}

#[test]
fn an_instant_message_is_told_and_acknowledged_as_delivered() {
    let c = messenger();
    let guid = [9u8; 16];
    let at = 1_700_000_000u64.to_be_bytes();
    let im = frame(
        811,
        0,
        false,
        fields(&[
            (tag::FRIEND_LOGIN, b"alice"),
            (tag::MESSAGE_GUID, &guid),
            (tag::MESSAGE_BODY, b"lunch?\rnow"),
            (tag::MESSAGE_TIMESTAMP, &at),
        ]),
    );
    unsafe { pushed(c.0, 811, &im) };
    assert_eq!(
        told(),
        [Event::Im(Im {
            from: b"alice".to_vec(),
            guid,
            body: "lunch?\nnow".into(),
            sent_at: 1_700_000_000,
        })]
    );
    let (opcode, ack_trans, acked) = sent().remove(0);
    assert_eq!(
        (opcode, acked),
        (
            812,
            vec![
                (tag::MESSAGE_GUID, guid.to_vec()),
                (tag::ACK_TYPE, vec![0, 1]),
                (tag::FRIEND_LOGIN, b"alice".to_vec())
            ]
        )
    );
    // Its refusal, which the user never asked for, is nobody's news.
    unsafe { reply(c.0, ack_trans, &frame(TASK, ack_trans, true, fields(&[]))) };
    assert_eq!(told(), []);
    // One that names no sender is no message.
    let bare = frame(811, 0, false, fields(&[(tag::MESSAGE_BODY, b"?")]));
    unsafe { pushed(c.0, 811, &bare) };
    assert_eq!((told(), sent()), (vec![], vec![]));
}

#[test]
fn a_sent_message_moves_on_with_its_answer_and_its_receipts() {
    let c = messenger();
    let guid = unsafe { crate::send::friends::im(c.0, b"bob", "hi").unwrap() };
    let (_, trans, _) = sent().remove(0);
    let queued = [0, ReasonCode::OfflineQueued.wire() as u8];
    unsafe {
        reply(
            c.0,
            trans,
            &frame(TASK, trans, false, fields(&[(tag::REASON_CODE, &queued)])),
        )
    };
    let receipt = |from: &'static [u8], ack: AckType| {
        frame(
            812,
            0,
            false,
            fields(&[
                (tag::MESSAGE_GUID, &guid),
                (tag::ACK_TYPE, &[0, ack.wire() as u8]),
                (tag::FRIEND_LOGIN, from),
            ]),
        )
    };
    unsafe {
        pushed(c.0, 812, &receipt(b"eve", AckType::Read));
        pushed(c.0, 812, &receipt(b"bob", AckType::Delivered));
        pushed(c.0, 812, &receipt(b"bob", AckType::Read));
    }
    let delivery = |d| Event::Delivery {
        to: b"bob".to_vec(),
        guid,
        delivery: d,
    };
    assert_eq!(
        told(),
        [
            delivery(Delivery::Queued),
            delivery(Delivery::Delivered),
            delivery(Delivery::Read)
        ]
    );
}

#[test]
fn a_refusal_says_why_and_fails_the_message_it_answers() {
    let c = messenger();
    let guid = unsafe { crate::send::friends::im(c.0, b"zed", "hi").unwrap() };
    let (_, trans, _) = sent().remove(0);
    let why = [0, ReasonCode::NotFriends.wire() as u8];
    let refusal = frame(
        TASK,
        trans,
        true,
        fields(&[(tag::REASON_CODE, &why), (tag::TASK_ERROR, b"Not friends.")]),
    );
    unsafe { reply(c.0, trans, &refusal) };
    assert_eq!(
        told(),
        [
            Event::Delivery {
                to: b"zed".to_vec(),
                guid,
                delivery: Delivery::Failed
            },
            Event::Refused {
                trans,
                request: 810,
                reason: Some(ReasonCode::NotFriends),
                text: Some("Not friends.".into())
            }
        ]
    );
    // Answered once: a second reply on the trans is nobody's.
    unsafe { reply(c.0, trans, &refusal) };
    assert_eq!(told(), []);
}

#[test]
fn find_user_answers_with_the_entry_or_with_nobody() {
    let c = messenger();
    let found = unsafe { crate::send::friends::find(c.0, b"alice").unwrap() };
    let missing = unsafe { crate::send::friends::find(c.0, b"nobody").unwrap() };
    let alice = [entry(b"alice", None, Some(P::Online))];
    let not_found = [0, ReasonCode::AccountNotFound.wire() as u8];
    unsafe {
        reply(c.0, found, &frame(TASK, found, false, entries(&alice)));
        reply(
            c.0,
            missing,
            &frame(
                TASK,
                missing,
                true,
                fields(&[(tag::REASON_CODE, &not_found)]),
            ),
        );
    }
    let events = told();
    match &events[..] {
        [Event::Found {
            trans: t1,
            friend: Some(f),
        }, Event::Found {
            trans: t2,
            friend: None,
        }] => {
            assert_eq!(
                (*t1, f.login.as_slice(), *t2),
                (found, &b"alice"[..], missing)
            );
        }
        other => panic!("{other:?}"),
    }
}

#[test]
fn typing_is_told_and_a_push_this_client_does_not_take_is_dropped() {
    let c = messenger();
    let typing = frame(
        813,
        0,
        false,
        fields(&[(tag::FRIEND_LOGIN, b"bob"), (tag::TYPING_STATE, &[0, 1])]),
    );
    // Message Requests Changed, which a client that never listed its
    // requests is not sent, and is not this client's if it is.
    let requests = frame(832, 0, false, fields(&[(0x0653, &[0, 0, 0, 1])]));
    unsafe {
        pushed(c.0, 813, &typing);
        pushed(c.0, 832, &requests);
    }
    assert_eq!(
        told(),
        [Event::Typing {
            from: b"bob".to_vec(),
            typing: true
        }]
    );
}

#[test]
fn a_reply_that_cannot_be_read_is_said_to_be_none() {
    let c = messenger();
    let (roster, find) = unsafe {
        (
            crate::send::friends::roster(c.0).unwrap(),
            crate::send::friends::find(c.0, b"alice").unwrap(),
        )
    };
    // A field cut short.
    let cut = |trans| {
        let mut f = frame(TASK, trans, false, fields(&[(tag::FRIEND_LOGIN, b"alice")]));
        f.truncate(f.len() - 2);
        f
    };
    unsafe {
        reply(c.0, roster, &cut(roster));
        reply(c.0, find, &cut(find));
    }
    let none = |trans, request| Event::Refused {
        trans,
        request,
        reason: None,
        text: None,
    };
    assert_eq!(told(), [none(roster, 800), none(find, 822)]);
}

#[test]
fn a_connection_closing_lets_go_of_its_roster_and_its_requests() {
    let c = messenger();
    let guid = unsafe { crate::send::friends::im(c.0, b"bob", "hi").unwrap() };
    let trans = unsafe { crate::send::friends::roster(c.0).unwrap() };
    unsafe {
        pushed(
            c.0,
            801,
            &frame(
                801,
                0,
                false,
                entries(&[entry(b"bob", Some(S::Accepted), None)]),
            ),
        )
    };
    told();
    unsafe { forget(c.0) };
    // The message nobody answered is lost with it.
    assert_eq!(
        told(),
        [Event::Delivery {
            to: b"bob".to_vec(),
            guid,
            delivery: Delivery::Failed
        }]
    );
    unsafe { reply(c.0, trans, &frame(TASK, trans, false, entries(&[]))) };
    assert_eq!(told(), []);
    assert!(with_roster(c.0, |r| r.get(b"bob").is_none()));
}
