//! What Reaction Changed and the replies to Get Reactions become: the
//! events the `reactions` signal would carry, through a recording double of
//! its emit, and the requests asked again, through the send doubles. Frames
//! are built as a server builds them.

use hxproto::reactions::{Key, Mine, Summary, TARGETS_LINES};
use hxsession::cap::{REACTIONS, STRUCTURED_CHAT, TEXT_ENCODING};

use super::*;
use crate::recv::friends::tests::{fields, frame};
use crate::send::friends::tests::{sent, woken, Conn};
use crate::send::reactions::{pace, react, refresh, retry, withdraw};
use hxproto::structured_chat::NOT_ALLOWED;
use std::time::{Duration, Instant};

thread_local! {
    static TOLD: RefCell<Vec<Event>> = const { RefCell::new(Vec::new()) };
}

pub(super) unsafe fn gtkhx_session_emit_reactions(_s: *mut c_void, _htlc: *mut c_void, e: &Event) {
    TOLD.with(|t| t.borrow_mut().push(e.clone()));
}

pub(super) unsafe fn gtkhx_session_get_default() -> *mut c_void {
    std::ptr::null_mut()
}

fn told() -> Vec<Event> {
    TOLD.with(|t| t.take())
}

const TASK: u32 = 0x0001_0000;
const AGREED: u64 = TEXT_ENCODING | STRUCTURED_CHAT | REACTIONS;
const THUMBS: &str = "\u{1F44D}";

fn lines() -> Option<Limits> {
    Some(Limits {
        targets: TARGETS_LINES,
        max_keys: None,
        max_per_user: None,
    })
}

/// A connection agreed to `caps`, its login having said `limits`.
fn conn(caps: u64, limits: Option<Limits>) -> Conn {
    let c = Conn::with_caps(caps);
    agreed(c.0, limits);
    c
}

fn line(id: u64) -> Target {
    Target::Line { channel: 0, id }
}

fn packed(id: u64) -> Vec<u8> {
    line(id).pack().unwrap()
}

/// A thumbs up `n` times, packed.
fn summary(n: u32) -> Vec<u8> {
    let key = Key::Emoji(THUMBS.into()).pack().unwrap();
    [
        &[0, 1][..],
        &n.to_be_bytes(),
        &(key.len() as u16).to_be_bytes(),
        &key,
    ]
    .concat()
}

/// What line `id` holds now: a thumbs up `n` times, ours among them or
/// not, or nothing.
fn now(id: u64, held: Option<(u32, bool)>) -> Event {
    Event::Now {
        target: line(id),
        reactions: held.map(|(n, ours)| {
            let summary = Summary {
                entries: vec![(Key::Emoji(THUMBS.into()), n)],
            };
            (summary, Mine(if ours { vec![0] } else { vec![] }))
        }),
    }
}

/// The targets each request written since last asked names, by trans.
fn asked_after() -> Vec<(u32, usize)> {
    sent()
        .into_iter()
        .map(|(opcode, trans, f)| {
            assert_eq!(opcode, 712);
            (trans, f.iter().filter(|(t, _)| *t == tag::TARGET).count())
        })
        .collect()
}

#[test]
fn a_reaction_changed_tells_each_line_it_names_where_the_session_has_reactions() {
    let im = Target::Im {
        guid: [7; 16],
        sender: b"bob".to_vec(),
    }
    .pack()
    .unwrap();
    let key = Key::Emoji(THUMBS.into()).pack().unwrap();
    let f = frame(
        711,
        0,
        false,
        fields(&[
            (tag::TARGET, &packed(1)),
            (tag::REACTION_ACTION, &[0, 4]),
            (tag::REACTION_SUMMARY, &summary(2)),
            (tag::REACTION_MINE, &[1, 0]),
            (tag::TARGET, &packed(2)),
            (tag::REACTION_ACTION, &[0, 3]),
            (tag::TARGET, &im),
            (tag::REACTION_ACTION, &[0, 1]),
            (tag::REACTION_KEY, &key),
            (tag::FRIEND_LOGIN, b"bob"),
        ]),
    );
    told();
    for (case, c, opcode, want) in [
        (
            "reactions",
            conn(AGREED, lines()),
            711,
            vec![now(1, Some((2, true))), now(2, None)],
        ),
        ("a notification", conn(AGREED, lines()), 718, vec![]),
        ("the bit without the kinds", conn(AGREED, None), 711, vec![]),
        (
            "the kinds without the bit",
            conn(TEXT_ENCODING | STRUCTURED_CHAT, lines()),
            711,
            vec![],
        ),
    ] {
        unsafe { pushed(c.0, opcode, &f) };
        assert_eq!(told(), want, "{case}");
    }
}

#[test]
fn a_get_reactions_reply_tells_each_line_asked_after_in_order() {
    let c = conn(AGREED, lines());
    sent();
    told();
    unsafe { refresh(c.0, &[line(1), line(2), line(3)]) };
    let [(trans, 3)] = asked_after()[..] else {
        panic!("not one request after three lines")
    };
    let f = frame(
        TASK,
        trans,
        false,
        fields(&[
            (tag::TARGET, &packed(3)),
            (tag::REACTION_SUMMARY, &summary(1)),
            (tag::TARGET, &packed(1)),
            (tag::REACTION_SUMMARY, &summary(4)),
            (tag::REACTION_MINE, &[1, 0]),
        ]),
    );
    unsafe { reply(c.0, trans, &f) };
    // One the reply leaves out holds nothing the reader may see.
    assert_eq!(
        told(),
        [
            now(1, Some((4, true))),
            now(2, None),
            now(3, Some((1, false)))
        ]
    );
    unsafe { reply(c.0, trans, &f) };
    assert_eq!(told(), [], "answered twice");
}

#[test]
fn lines_are_asked_after_fifty_at_once_and_a_reply_too_big_for_its_frame_in_halves() {
    let c = conn(AGREED, lines());
    sent();
    told();
    let many: Vec<Target> = (1..=51).map(line).collect();
    unsafe { refresh(c.0, &many) };
    let [(fifty, 50), (one, 1)] = asked_after()[..] else {
        panic!("not fifty and one")
    };
    let refused = |trans: u32, code: u16| {
        frame(
            TASK,
            trans,
            true,
            fields(&[(tag::CHAT_REASON, &code.to_be_bytes())]),
        )
    };
    unsafe { reply(c.0, fifty, &refused(fifty, TOO_MANY_TARGETS)) };
    let [(half, 25), (_, 25)] = asked_after()[..] else {
        panic!("not halved")
    };
    for (trans, code) in [(one, TOO_MANY_TARGETS), (half, 2)] {
        unsafe { reply(c.0, trans, &refused(trans, code)) };
        assert_eq!(asked_after(), [], "{code} asked again");
    }
    assert_eq!(told(), []);

    // Nothing goes out without reactions, nor after a kind not taken.
    let ephemeral = Target::Ephemeral { chat: 0, id: 1 };
    for (c, targets) in [
        (conn(AGREED, None), &many[..1]),
        (conn(AGREED, lines()), &[ephemeral][..]),
    ] {
        unsafe { refresh(c.0, targets) };
        assert_eq!(asked_after(), []);
    }
}

fn thumbs() -> Key {
    Key::Emoji(THUMBS.into())
}

/// The lines each Send Reaction written since last asked reacts to, with
/// its trans.
fn reacted_to() -> Vec<(u32, Target)> {
    sent()
        .into_iter()
        .map(|(opcode, trans, f)| {
            assert_eq!(opcode, 710);
            let (_, t) = f.iter().find(|(t, _)| *t == tag::TARGET).unwrap();
            (trans, Target::parse(t).unwrap())
        })
        .collect()
}

#[test]
fn reactions_go_at_the_drafts_rate_and_one_waiting_may_be_taken_back() {
    let c = conn(AGREED, lines());
    sent();
    woken();
    let start = Instant::now();
    for id in 1..=12 {
        assert!(unsafe { react(c.0, line(id), thumbs(), true) });
    }
    let first: Vec<Target> = reacted_to().into_iter().map(|(_, t)| t).collect();
    assert_eq!(first, (1..=10).map(line).collect::<Vec<_>>(), "the burst");
    let timers = woken();
    assert!(
        !timers.is_empty() && timers.iter().all(|at| *at > start),
        "a timer for the rest"
    );

    assert!(unsafe { withdraw(c.0, &line(12), &thumbs()) });
    assert!(
        !unsafe { withdraw(c.0, &line(12), &thumbs()) },
        "taken twice"
    );
    assert!(
        !unsafe { withdraw(c.0, &line(1), &thumbs()) },
        "one gone already"
    );

    let heart = |e: &str| Key::Emoji(e.into());
    unsafe { react(c.0, line(14), heart("\u{2764}\u{FE0F}"), true) };
    assert!(
        unsafe { withdraw(c.0, &line(14), &heart("\u{2764}")) },
        "a key in another spelling"
    );

    let second = Instant::now() + Duration::from_millis(1001);
    unsafe { pace(c.0, second) };
    assert_eq!(
        reacted_to().into_iter().map(|(_, t)| t).collect::<Vec<_>>(),
        [line(11)]
    );
    unsafe { pace(c.0, second) };
    assert_eq!(reacted_to(), [], "nothing left");

    // What waits goes with the connection.
    unsafe { react(c.0, line(13), thumbs(), true) };
    forget(c.0);
    unsafe { pace(c.0, second + Duration::from_secs(10)) };
    assert_eq!(reacted_to(), [], "sent after the connection went");

    // Nothing goes without reactions.
    let none = conn(AGREED, None);
    unsafe { react(none.0, line(1), thumbs(), true) };
    assert_eq!(sent(), []);
}

#[test]
fn a_send_reaction_reply_says_what_was_made_or_why_not() {
    let refused = |trans: u32, list: &[(u16, &[u8])]| frame(TASK, trans, true, fields(list));
    let acted = |n: u32, add: bool| Event::Acted {
        target: line(1),
        key: thumbs(),
        add,
        count: Some(n),
    };
    let no = |reason: Option<u16>, error: Option<&str>| Event::Refused {
        target: line(1),
        key: thumbs(),
        reason,
        error: error.map(Into::into),
    };
    let not_allowed = NOT_ALLOWED.to_be_bytes();
    for (case, add, answer, want) in [
        (
            "made, with the count after",
            true,
            frame(
                TASK,
                0,
                false,
                fields(&[(tag::REACTION_COUNT, &[0, 0, 0, 3])]),
            ),
            vec![acted(3, true)],
        ),
        (
            "taken back",
            false,
            frame(
                TASK,
                0,
                false,
                fields(&[(tag::REACTION_COUNT, &[0, 0, 0, 0])]),
            ),
            vec![acted(0, false)],
        ),
        (
            "refused with a code and words",
            true,
            refused(
                0,
                &[(tag::CHAT_REASON, &not_allowed), (tag::TASK_ERROR, b"No.")],
            ),
            vec![no(Some(NOT_ALLOWED), Some("No."))],
        ),
        (
            "refused with words alone",
            true,
            refused(0, &[(tag::TASK_ERROR, b"Server error.")]),
            vec![no(None, Some("Server error."))],
        ),
    ] {
        let c = conn(AGREED, lines());
        sent();
        told();
        unsafe { react(c.0, line(1), thumbs(), add) };
        let [(trans, _)] = reacted_to()[..] else {
            panic!("{case}: not sent")
        };
        unsafe { reply(c.0, trans, &answer) };
        assert_eq!(told(), want, "{case}");
    }

    // Told to slow down, the view hears it put off, and nothing goes for a
    // second; asked again and told so again, it is refused.
    let c = conn(AGREED, lines());
    sent();
    woken();
    unsafe { react(c.0, line(1), thumbs(), true) };
    let slow = RATE_LIMITED.to_be_bytes();
    let [(trans, _)] = reacted_to()[..] else {
        panic!("not sent")
    };
    unsafe { reply(c.0, trans, &refused(trans, &[(tag::CHAT_REASON, &slow)])) };
    let put_off = Event::PutOff {
        target: line(1),
        key: thumbs(),
        add: true,
    };
    assert_eq!((told(), reacted_to()), (vec![put_off], vec![]), "at once");
    unsafe { retry(c.0, line(1), thumbs(), true) };
    assert_eq!(reacted_to(), [], "asked again at once");
    let [at] = woken()[..] else {
        panic!("no timer")
    };
    assert!(
        at >= Instant::now() + Duration::from_millis(900),
        "not a second on"
    );
    unsafe { pace(c.0, at) };
    let [(trans, _)] = reacted_to()[..] else {
        panic!("not asked again")
    };
    unsafe { reply(c.0, trans, &refused(trans, &[(tag::CHAT_REASON, &slow)])) };
    assert_eq!(told(), [no(Some(RATE_LIMITED), None)], "twice");
}
