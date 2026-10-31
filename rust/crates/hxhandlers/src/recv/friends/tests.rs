//! What the friend family's pushes and replies become: the roster a real
//! connection holds and the events the `messaging` signal would carry,
//! through a recording double of its emit. Frames are built as a server
//! builds them, with hxproto's own encoder.

use std::cell::RefCell;

use gtkhx_core::messaging::with_roster;
use hxproto::messaging::{PresenceState as P, RosterEntry, RosterState as S};
use hxproto::wire::Encoder;

use super::*;
use crate::send::friends::tests::{sent, woken, Conn};

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
    // Message Requests Changed, which a server that serves no requests
    // never sends, and is nothing from one.
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

/// What the listing is answered with: `takes` and `dropped` before any
/// group, as on a first page, then each group's fields.
fn listing(trans: u32, before: &[(u16, &[u8])], groups: &[(u16, &[u8])]) -> Vec<u8> {
    let all: Vec<(u16, &[u8])> = before.iter().chain(groups).copied().collect();
    frame(TASK, trans, false, fields(&all))
}

#[test]
fn the_listing_is_read_page_by_page_paced_and_told_once() {
    let c = Conn::serving_requests(0);
    sent();
    told();
    woken();
    let asked = std::time::Instant::now();
    unsafe { crate::send::friends::requests(c.0) };
    let (opcode, first, cursor) = sent().remove(0);
    assert_eq!((opcode, cursor), (829, vec![]));
    let t1 = 1_700_000_000u64.to_be_bytes();
    let page1 = listing(
        first,
        &[
            (tag::TAKES_REQUESTS, &[0, 1]),
            (tag::REQUEST_COUNT, &[0, 0, 0, 2]),
            (tag::REQUEST_CURSOR, &[0, 0, 0, 1]),
        ],
        &[
            (tag::FRIEND_LOGIN, b"alice"),
            (tag::MESSAGE_GUID, &[1; 16]),
            (tag::MESSAGE_BODY, b"lunch\rnow"),
            (tag::MESSAGE_TIMESTAMP, &t1),
        ],
    );
    unsafe { reply(c.0, first, &page1) };
    // Nothing told, and the next page not asked for until a second on.
    assert_eq!((told(), sent()), (vec![], vec![]));
    let [at] = woken()[..] else { panic!() };
    assert!(at >= asked + model::requests::PAGE_GAP);
    unsafe { crate::send::friends::pace(c.0, at) };
    let (opcode, second, cursor) = sent().remove(0);
    assert_eq!(
        (opcode, cursor),
        (829, vec![(tag::REQUEST_CURSOR, vec![0, 0, 0, 1])])
    );
    let page2 = listing(
        second,
        &[],
        &[
            (tag::FRIEND_LOGIN, b"\xe9ve"),
            (tag::MESSAGE_GUID, &[2; 16]),
            (tag::MESSAGE_BODY, b"hi"),
            (tag::MESSAGE_TIMESTAMP, &t1),
            (tag::FRIEND_LOGIN, b"bob"),
            (tag::ROSTER_STATE, &[0, 3]),
        ],
    );
    unsafe { reply(c.0, second, &page2) };
    assert_eq!(told(), [Event::Requests]);
    with_roster(c.0, |m| {
        let r = &m.requests;
        assert_eq!((r.count, r.dropped, r.takes), (2, 2, Some(true)));
        let alice = &r.senders[0];
        assert_eq!(
            (alice.login.as_slice(), alice.messages[0].body.as_str()),
            (&b"alice"[..], "lunch\nnow")
        );
        assert_eq!(r.senders[1].login, b"\xe9ve");
        assert!(r.approvals.contains(&b"bob"[..]));
    });
    assert_eq!(woken(), []);
}

#[test]
fn a_refused_listing_is_told_and_not_asked_for_again_unasked() {
    let c = Conn::serving_requests(0);
    sent();
    told();
    woken();
    unsafe { crate::send::friends::requests(c.0) };
    let (_, trans, _) = sent().remove(0);
    unsafe { reply(c.0, trans, &frame(TASK, trans, true, fields(&[]))) };
    assert_eq!(
        told(),
        [Event::Refused {
            trans,
            request: 829,
            reason: None,
            text: None
        }]
    );
    assert_eq!((sent(), woken()), (vec![], vec![]));
    // Asked again, it goes once the pacing allows.
    unsafe { crate::send::friends::requests(c.0) };
    assert_eq!((sent().len(), woken().len()), (0, 1));
}

#[test]
fn a_change_in_the_requests_is_told_with_its_count() {
    let unbound = messenger();
    let c = Conn::serving_requests(0);
    let changed = frame(
        832,
        0,
        false,
        fields(&[(tag::REQUEST_COUNT, &[0, 0, 0, 3])]),
    );
    unsafe {
        pushed(unbound.0, 832, &changed);
        pushed(c.0, 832, &changed);
    }
    assert_eq!(told(), [Event::Requests]);
    assert_eq!(with_roster(c.0, |m| m.requests.count), 3);
    // Nothing is listed for it: the view asks, where it shows them.
    assert_eq!(sent(), []);
}

#[test]
fn ending_an_approval_lists_the_requests_again_and_a_clearing_says_what_went() {
    let c = Conn::serving_requests(0);
    sent();
    told();
    let ok = |trans| frame(TASK, trans, false, fields(&[]));
    let (approve, end) = unsafe {
        (
            crate::send::friends::answer_request(c.0, b"bob", RequestAction::Approve).unwrap(),
            crate::send::friends::answer_request(c.0, b"eve", RequestAction::EndApproval).unwrap(),
        )
    };
    sent();
    // An approval is followed by the server's own Message Requests
    // Changed; an end by nothing, so it is listed here.
    unsafe { reply(c.0, approve, &ok(approve)) };
    assert_eq!(sent(), []);
    unsafe { reply(c.0, end, &ok(end)) };
    let opcodes: Vec<u32> = sent().into_iter().map(|(o, _, _)| o).collect();
    assert_eq!(opcodes, [829]);

    let clear = unsafe { crate::send::friends::clear_requests(c.0, Some(1), None).unwrap() };
    let counts = frame(
        TASK,
        clear,
        false,
        fields(&[
            (tag::REQUEST_COUNT, &[0, 0, 0, 4]),
            (tag::FRIEND_REQUEST_COUNT, &[0, 0, 0, 1]),
        ]),
    );
    unsafe { reply(c.0, clear, &counts) };
    assert_eq!(
        told(),
        [Event::Cleared {
            trans: clear,
            requests: 4,
            friend_requests: 1
        }]
    );
}

#[test]
fn a_message_held_as_a_request_is_said_to_be() {
    let c = messenger();
    let guid = unsafe { crate::send::friends::im(c.0, b"zed", "hi").unwrap() };
    let (_, trans, _) = sent().remove(0);
    let requested = [0, ReasonCode::Requested.wire() as u8];
    let answer = frame(
        TASK,
        trans,
        false,
        fields(&[(tag::REASON_CODE, &requested)]),
    );
    unsafe { reply(c.0, trans, &answer) };
    assert_eq!(
        told(),
        [Event::Delivery {
            to: b"zed".to_vec(),
            guid,
            delivery: Delivery::Requested
        }]
    );
}

/// A page's text in the connection's encoding: Mac Roman unless UTF-8
/// was negotiated, even where its bytes would read as UTF-8.
#[test]
fn a_page_is_read_in_the_connections_encoding() {
    for (caps, body) in [
        (0, "caf\u{221a}\u{a9}"),
        (hxsession::cap::TEXT_ENCODING, "caf\u{e9}"),
    ] {
        let c = Conn::serving_requests(caps);
        sent();
        unsafe { crate::send::friends::requests(c.0) };
        let (_, trans, _) = sent().remove(0);
        let at = 1_700_000_000u64.to_be_bytes();
        let page = listing(
            trans,
            &[],
            &[
                (tag::FRIEND_LOGIN, b"alice"),
                (tag::MESSAGE_GUID, &[1; 16]),
                (tag::MESSAGE_BODY, b"caf\xc3\xa9"),
                (tag::MESSAGE_TIMESTAMP, &at),
            ],
        );
        unsafe { reply(c.0, trans, &page) };
        let got = with_roster(c.0, |m| m.requests.senders[0].messages[0].body.clone());
        assert_eq!(got, body, "caps {caps:#x}");
    }
}

#[test]
fn a_page_after_the_connection_let_go_is_nobodys() {
    let c = Conn::serving_requests(0);
    sent();
    told();
    unsafe { crate::send::friends::requests(c.0) };
    let (_, trans, _) = sent().remove(0);
    unsafe { forget(c.0) };
    let page = listing(trans, &[], &[(tag::FRIEND_LOGIN, b"alice")]);
    unsafe { reply(c.0, trans, &page) };
    assert_eq!((told(), sent()), (vec![], vec![]));
    assert!(with_roster(c.0, |m| m.requests.senders.is_empty()));
}
