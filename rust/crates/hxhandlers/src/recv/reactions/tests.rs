//! What Reaction Changed and the replies to Get Reactions become: the
//! events the `reactions` signal would carry, through a recording double of
//! its emit, and the requests asked again, through the send doubles. Frames
//! are built as a server builds them.

use hxproto::reactions::{Key, Mine, Summary, TARGETS_LINES};
use hxsession::cap::{REACTIONS, STRUCTURED_CHAT, TEXT_ENCODING};

use super::*;
use crate::recv::friends::tests::{fields, frame};
use crate::send::friends::tests::{sent, Conn};
use crate::send::reactions::refresh;

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
