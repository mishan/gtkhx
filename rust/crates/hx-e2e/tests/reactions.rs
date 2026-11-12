//! The Reactions draft (capability bit 19) as GtkHx speaks it:
//! `hxrequest::reactions`' requests, and Reaction Changed and the replies
//! as hxnet hands them to production's receive handlers, the session's own
//! `Unhandled` and `Reply`, read with hxproto's parsers; and a history
//! entry's reactions, as the session reads them.
#![cfg(feature = "rig")]

use hx_e2e::client::CAP_TEXT_ENCODING;
use hx_e2e::{servers_with, unique_name, Cap, Client, Reply, Server};
use hxmodel::reactions::LineReactions;
use hxnet::Event;
use hxproto::messages::tag;
use hxproto::messaging::{RosterState, CAP_MESSAGING};
use hxproto::reactions::{
    parse_changed, parse_got, Group, Key, Mine, Summary, ADD, CLEAR, INVALID_KEY, REMOVE,
    TARGETS_EPHEMERAL, TARGETS_IMS, TARGETS_LINES, TARGETS_STORED_PMS,
};
use hxproto::structured_chat::{NOT_ALLOWED, NO_SUCH_TARGET};
use hxproto::target::Target;
use hxproto::wire::ChunkIter;
use hxrequest::messaging as im;
use hxrequest::reactions as req;
use hxrequest::Request;
use hxsession::cap::{CHAT_HISTORY, REACTIONS, STRUCTURED_CHAT};
use hxsession::{Expect, Handled, HistoryEntry};

const OFFER: u64 = CAP_TEXT_ENCODING | CHAT_HISTORY | STRUCTURED_CHAT | REACTIONS;
const THUMBS: &str = "\u{1F44D}";
/// A live line's entry flag: its id is its chat's own count's.
const EPHEMERAL: u16 = 0x0008;
/// The rig's room, as `tests/hxd-ng/conf/rooms.toml` numbers it.
const ROOM: u32 = 2754100983;

fn thumbs() -> Key {
    Key::Emoji(THUMBS.into())
}

/// `login` (its password is its name, none for the guest) offering
/// `caps`, its session handling chat.
fn chatter(s: &'static Server, login: &str, caps: u64) -> Client {
    let nick: String = unique_name(if login.is_empty() { "guest" } else { login })
        .chars()
        .take(31)
        .collect();
    let password = (!login.is_empty()).then_some(login);
    Client::login_handling(s, login, password, caps, Handled::CHAT, &nick)
        .unwrap_or_else(|e| panic!("{e}"))
}

/// `login`, with reactions.
fn reactor(s: &'static Server, login: &str) -> Client {
    let c = chatter(s, login, OFFER);
    let limits = c.server_info().reactions;
    assert!(
        limits.is_some_and(|l| l.targets & TARGETS_LINES != 0),
        "{login}: no reactions on lines: {limits:?}"
    );
    c
}

/// Say `text` in the lobby; the line, as its id names it.
fn say(c: &mut Client, text: &str) -> Target {
    let e = heard_said(c, 0, text);
    assert_eq!(e.flags & EPHEMERAL, 0, "an id the history did not give");
    Target::Line {
        channel: 0,
        id: e.message_id,
    }
}

/// Say `text` in chat `cid`, 0 the lobby: the line's entry, from its echo.
fn heard_said(c: &mut Client, cid: u32, text: &str) -> HistoryEntry {
    let mut chunks = vec![(tag::BODY, text.as_bytes().to_vec())];
    if cid != 0 {
        chunks.push((tag::CHAT_ID, cid.to_be_bytes().to_vec()));
    }
    c.send(&Request {
        opcode: 105,
        chunks,
    });
    heard_line(c, cid, text)
}

/// The entry of the line `text` that `c` hears in chat `cid`.
fn heard_line(c: &mut Client, cid: u32, text: &str) -> HistoryEntry {
    loop {
        if let Event::Session(hxsession::Event::Chat {
            cid: at,
            line: Some(e),
            ..
        }) = c.next_event().unwrap_or_else(|e| panic!("{e}"))
        {
            if at == cid && e.text == text {
                return e;
            }
        }
    }
}

/// Join chat `cid`, as accepting its invitation does.
fn join(c: &mut Client, cid: u32) {
    let r = c.request(&Request {
        opcode: 115,
        chunks: vec![(tag::CHAT_ID, cid.to_be_bytes().to_vec())],
    });
    assert!(
        !r.is_error(),
        "{}: joining {cid}: {}",
        c.server().name,
        r.error_text()
    );
}

fn reason(r: &Reply) -> Option<u16> {
    r.chunk(tag::CHAT_REASON)
        .and_then(|c| c.try_into().ok())
        .map(u16::from_be_bytes)
}

/// Add or remove a thumbs up on `target`: the key's count after.
fn react(c: &mut Client, target: &Target, action: u16) -> u32 {
    let r = c.request(&req::send(target, action, Some(&thumbs()), None).unwrap());
    assert!(!r.is_error(), "{}", r.error_text());
    ChunkIter::over_message(&r.raw, r.raw.len())
        .find(|c| c.tag == tag::REACTION_COUNT)
        .expect("no count")
        .as_uint()
}

/// Wait for `c` to be told in Reaction Changed that `target` holds `want`,
/// or that it was cleared, skipping what else it is told; a Reaction
/// Changed arriving whole, as a frame, fails the test.
fn told(c: &mut Client, target: &Target, want: Option<(Summary, Mine)>) {
    loop {
        match c.next_event().unwrap_or_else(|e| panic!("no 711: {e}")) {
            Event::Session(hxsession::Event::Unhandled { opcode: 711, frame }) => {
                for g in parse_changed(ChunkIter::over_message(&frame, frame.len())) {
                    match (g, &want) {
                        (
                            Group::Summary {
                                target: t,
                                summary,
                                mine,
                            },
                            Some(want),
                        ) if t == *target && (&summary, &mine) == (&want.0, &want.1) => return,
                        (Group::Cleared { target: t }, None) if t == *target => return,
                        _ => {}
                    }
                }
            }
            Event::Frame(f) if f.header.type_ == 711 => panic!("711 came as a frame"),
            _ => {}
        }
    }
}

fn summary(n: u32) -> Summary {
    Summary {
        entries: vec![(thumbs(), n)],
    }
}

/// Public chat's newest lines, as GtkHx asks for them at login.
fn history(c: &mut Client) -> Vec<HistoryEntry> {
    let ask = hxrequest::chat::history(0, 0, 0, 50);
    let t = c.send_expecting(&ask, Some(Expect::ChatHistory { cid: 0 }));
    loop {
        match c.next_event().unwrap_or_else(|e| panic!("{e}")) {
            Event::Session(hxsession::Event::ChatHistory { trans, entries, .. }) if trans == t => {
                return entries
            }
            Event::Session(hxsession::Event::Failed { trans, reason }) if trans == t => {
                panic!("history refused: {reason:?}")
            }
            _ => {}
        }
    }
}

/// Only where the draft is served is bit 19 confirmed, with the target
/// kinds beside it.
#[test]
fn bit_19_is_confirmed_only_where_it_is_served() {
    for s in servers_with(&[]) {
        let c = Client::guest(s, OFFER);
        assert_eq!(c.caps() & REACTIONS, 0, "{}", s.name);
        assert_eq!(c.server_info().reactions, None, "{}", s.name);
    }
    for s in servers_with(&[Cap::Reactions]) {
        reactor(s, "react-a");
    }
}

/// A reaction is counted, told to each reader with which keys are its
/// own, read back with who made it, and seeded into the next connect's
/// history; a reader that did not ask for bit 19 hears none of it.
#[test]
fn a_reaction_reaches_every_reader_that_asked_and_the_history() {
    for s in servers_with(&[Cap::Reactions]) {
        let mut a = reactor(s, "react-a");
        let mut b = reactor(s, "react-b");
        let mut plain = chatter(s, "react-b", OFFER & !REACTIONS);
        let line = say(&mut a, &unique_name("react to this"));

        assert_eq!(react(&mut a, &line, ADD), 1, "{}", s.name);
        told(&mut b, &line, Some((summary(1), Mine::default())));
        assert_eq!(react(&mut b, &line, ADD), 2, "{}", s.name);
        told(&mut a, &line, Some((summary(2), Mine(vec![0]))));

        // A line said once the others were told reaches the plain reader
        // after any Reaction Changed sent to it with theirs.
        let after = unique_name("after");
        say(&mut a, &after);
        loop {
            match plain.next_event().unwrap_or_else(|e| panic!("{e}")) {
                Event::Session(hxsession::Event::Chat { text, .. }) if text.contains(&after) => {
                    break
                }
                Event::Frame(f) if f.header.type_ == 711 => {
                    panic!("{}: told a plain reader", s.name)
                }
                Event::Session(hxsession::Event::Unhandled { opcode: 711, .. }) => {
                    panic!("{}: told a plain reader", s.name)
                }
                _ => {}
            }
        }

        let got = |c: &mut Client, key: Option<&Key>| {
            let r = c.request(&req::get(std::slice::from_ref(&line), key).unwrap());
            assert!(!r.is_error(), "{}", r.error_text());
            parse_got(ChunkIter::over_message(&r.raw, r.raw.len()))
        };
        let [g] = &got(&mut b, None)[..] else {
            panic!("{}: not one group", s.name)
        };
        assert_eq!(
            (&g.target, &g.summary, &g.mine),
            (&line, &summary(2), &Mine(vec![0]))
        );
        let [g] = &got(&mut a, Some(&thumbs()))[..] else {
            panic!("{}: not one group", s.name)
        };
        // Each by the nick it reacted under, which these accounts may not
        // choose: their names.
        assert_eq!(
            g.nicks,
            [b"react-b", b"react-a"],
            "{}: newest first",
            s.name
        );

        let id = |t: &Target| match t {
            Target::Line { id, .. } => *id,
            other => panic!("{other:?}"),
        };
        let seeded = |c: &mut Client| {
            history(c)
                .into_iter()
                .find(|e| e.message_id == id(&line))
                .unwrap_or_else(|| panic!("{}: the line is not in the history", s.name))
                .reactions
        };
        let mut again = reactor(s, "react-a");
        assert_eq!(
            seeded(&mut again),
            Some((summary(2), Mine(vec![0]))),
            "{}",
            s.name
        );

        assert_eq!(react(&mut a, &line, REMOVE), 1, "{}", s.name);
        told(&mut b, &line, Some((summary(1), Mine(vec![0]))));
    }
}

/// A guest may not react, and a key that is not one emoji is no key; each
/// says why with the draft's code.
#[test]
fn a_reaction_the_server_will_not_take_says_why() {
    for s in servers_with(&[Cap::Reactions]) {
        let mut a = reactor(s, "react-a");
        let line = say(&mut a, &unique_name("not for guests"));
        let mut guest = chatter(s, "", OFFER);
        for (who, key, code) in [
            (&mut guest, thumbs(), NOT_ALLOWED),
            (&mut a, Key::Emoji("ab".into()), INVALID_KEY),
        ] {
            let r = who.request(&req::send(&line, ADD, Some(&key), None).unwrap());
            assert!(r.is_error(), "{}: {key:?} taken", s.name);
            assert_eq!(reason(&r), Some(code), "{}: {}", s.name, r.error_text());
        }
    }
}

/// A moderator clears a line two others reacted to, and each of them is
/// told it was; one who is not a moderator is refused with the draft's
/// code.
#[test]
fn a_moderator_clears_a_line_and_nobody_else_may() {
    for s in servers_with(&[Cap::Reactions]) {
        let mut a = reactor(s, "react-a");
        let mut b = reactor(s, "react-b");
        let mut moderator = reactor(s, "react-mod");
        let line = say(&mut a, &unique_name("clear this"));
        react(&mut a, &line, ADD);
        assert_eq!(react(&mut b, &line, ADD), 2, "{}", s.name);
        let clear = req::send(&line, CLEAR, None, None).unwrap();

        let r = a.request(&clear);
        assert!(r.is_error(), "{}: a reader cleared it", s.name);
        assert_eq!(
            reason(&r),
            Some(NOT_ALLOWED),
            "{}: {}",
            s.name,
            r.error_text()
        );
        let r = moderator.request(&clear);
        assert!(!r.is_error(), "{}: {}", s.name, r.error_text());
        for c in [&mut a, &mut b] {
            told(c, &line, None);
        }
    }
}

/// GtkHx refuses one more reaction where the server would, by the limits
/// the login gave: each of two accounts reacts to a line with a new emoji
/// each time, as GtkHx sees the line from a Get Reactions, until GtkHx
/// would refuse, and the server then does with the same code.
#[test]
fn a_reaction_is_refused_where_the_logins_limits_say() {
    for s in servers_with(&[Cap::Reactions]) {
        let mut a = reactor(s, "react-a");
        let mut b = reactor(s, "react-b");
        let line = say(&mut a, &unique_name("react all over this"));
        let mut emoji = (0x1F600..0x1F650).map(|c| Key::Emoji(char::from_u32(c).unwrap().into()));
        for c in [&mut a, &mut b] {
            let l = c.server_info().reactions.unwrap();
            loop {
                let key = emoji.next().expect("never refused");
                let r = c.request(&req::get(std::slice::from_ref(&line), None).unwrap());
                let [g] = &parse_got(ChunkIter::over_message(&r.raw, r.raw.len()))[..] else {
                    panic!("{}: not one group", s.name)
                };
                let mut shown = LineReactions::default();
                shown.replace(g.summary.clone(), &g.mine);
                let refusal = shown.refusal(&key, l.max_keys, l.max_per_user);
                let r = c.request(&req::send(&line, ADD, Some(&key), None).unwrap());
                assert_eq!(
                    r.is_error(),
                    refusal.is_some(),
                    "{}: {}",
                    s.name,
                    r.error_text()
                );
                if refusal.is_some() {
                    assert_eq!(reason(&r), refusal, "{}", s.name);
                    break;
                }
            }
        }
    }
}

/// A room's line, which the classic wire shows as a private chat under the
/// room's chat id, is named kind 4 under that id: a reaction to it is
/// counted, told to the room's other members in that kind and read back
/// so. A private chat's line carries an id of the chat's own count, which
/// GtkHx offers no reaction to, and which the server keeps no line under.
#[test]
fn a_rooms_line_is_kind_4_under_its_chat_id() {
    for s in servers_with(&[Cap::Reactions]) {
        let mut a = reactor(s, "react-a");
        let mut b = reactor(s, "react-b");
        let limits = a.server_info().reactions.unwrap();
        assert_ne!(
            limits.targets & TARGETS_EPHEMERAL,
            0,
            "{}: no kind 4",
            s.name
        );
        for c in [&mut a, &mut b] {
            join(c, ROOM);
        }
        let text = unique_name("in the room");
        let e = heard_said(&mut a, ROOM, &text);
        assert_eq!(e.flags & EPHEMERAL, 0, "{}: a room's line not kept", s.name);
        let line = Target::Ephemeral {
            chat: ROOM,
            id: e.message_id,
        };
        assert_eq!(react(&mut a, &line, ADD), 1, "{}", s.name);
        told(&mut b, &line, Some((summary(1), Mine::default())));
        let r = b.request(&req::get(std::slice::from_ref(&line), None).unwrap());
        let [g] = &parse_got(ChunkIter::over_message(&r.raw, r.raw.len()))[..] else {
            panic!("{}: not one group: {}", s.name, r.error_text())
        };
        assert_eq!((&g.target, &g.summary), (&line, &summary(1)), "{}", s.name);

        let mut guest = chatter(s, "", OFFER);
        let a_uid = a.server_info().uid.expect("react-a's uid");
        let r = guest.request(&Request {
            opcode: 112,
            chunks: vec![(tag::UID, a_uid.to_be_bytes().to_vec())],
        });
        assert!(!r.is_error(), "{}: {}", s.name, r.error_text());
        let cid = u32::from_be_bytes(r.chunk(tag::CHAT_ID).unwrap().try_into().unwrap());
        join(&mut a, cid);
        let text = unique_name("between us");
        guest.send(&Request {
            opcode: 105,
            chunks: vec![
                (tag::BODY, text.as_bytes().to_vec()),
                (tag::CHAT_ID, cid.to_be_bytes().to_vec()),
            ],
        });
        let e = heard_line(&mut a, cid, &text);
        assert_ne!(
            e.flags & EPHEMERAL,
            0,
            "{}: a private chat's line kept",
            s.name
        );
        let unkept = Target::Ephemeral {
            chat: cid,
            id: e.message_id,
        };
        let r = a.request(&req::send(&unkept, ADD, Some(&thumbs()), None).unwrap());
        assert_eq!(
            reason(&r),
            Some(NO_SUCH_TARGET),
            "{}: {}",
            s.name,
            r.error_text()
        );
    }
}

/// `login` with reactions and the friend family, its session handling
/// `handled`.
fn messenger(s: &'static Server, login: &str, handled: Handled) -> Client {
    let offer = OFFER | CAP_MESSAGING;
    let c = Client::login_handling(s, login, Some(login), offer, handled, "hx-e2e")
        .unwrap_or_else(|e| panic!("{e}"));
    let limits = c.server_info().reactions;
    assert!(
        limits.is_some_and(|l| l.targets & TARGETS_IMS != 0),
        "{login}: no reactions on instant messages: {limits:?}"
    );
    c
}

/// `a` and `b` made friends, whatever an earlier run left between them.
fn befriend(a: &mut Client, a_login: &str, b: &mut Client, b_login: &str) {
    for (c, other) in [(&mut *a, b_login), (&mut *b, a_login)] {
        // Undoing nothing is refused, which is as good.
        c.request(&im::unblock(other.as_bytes()).unwrap());
        c.request(&im::remove_friend(other.as_bytes()).unwrap());
    }
    // Each asks the other, which is a friendship from sessions that
    // understand message requests, as listing them says: a request to
    // someone who removed the asker would be held, never told to them.
    let mut state = None;
    for (c, other) in [(&mut *a, b_login), (&mut *b, a_login)] {
        c.request(&im::message_requests(None).unwrap());
        let r = c.request(&im::add_friend(other.as_bytes(), None, true).unwrap());
        state = r.chunk(tag::ROSTER_STATE).map(<[u8]>::to_vec);
    }
    assert_eq!(
        state,
        Some(RosterState::Accepted.wire().to_be_bytes().to_vec())
    );
}

/// A message id no earlier run sent.
fn guid() -> [u8; 16] {
    let now = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .unwrap()
        .as_nanos();
    now.to_be_bytes()
}

/// The change `c` is told in Reaction Changed of `target`, skipping what
/// else it is told, and failing on one told before `before` is answered.
fn change(c: &mut Client, target: &Target, before: Option<u32>) -> (bool, Key, Vec<u8>) {
    let mut answered = before.is_none();
    loop {
        match c.next_event().unwrap_or_else(|e| panic!("no 711: {e}")) {
            Event::Session(hxsession::Event::Reply { trans, .. }) if Some(trans) == before => {
                answered = true
            }
            Event::Session(hxsession::Event::Unhandled { opcode: 711, frame }) => {
                for g in parse_changed(ChunkIter::over_message(&frame, frame.len())) {
                    if let Group::Change {
                        target: t,
                        add,
                        key,
                        from: Some(from),
                    } = g
                    {
                        if t == *target {
                            assert!(answered, "told before Get Roster's reply");
                            return (add, key, from);
                        }
                    }
                }
            }
            Event::Frame(f) if f.header.type_ == 711 => panic!("711 came as a frame"),
            _ => {}
        }
    }
}

/// A reaction to an instant message goes with the conversation's other
/// party, is answered with no count, and is told to that party as a change
/// naming who made it; one made while the party was away is told once it
/// is back, after Get Roster's reply.
#[test]
fn an_instant_messages_reaction_reaches_its_other_party() {
    for s in servers_with(&[Cap::Reactions, Cap::Messaging]) {
        let mut a = messenger(s, "react-a", Handled::USERS);
        let mut b = messenger(s, "react-b", Handled::NONE);
        befriend(&mut a, "react-a", &mut b, "react-b");
        let g = guid();
        let sent = b.request(&im::im_send(b"react-a", &g, b"react to this", true).unwrap());
        assert!(!sent.is_error(), "{}: {}", s.name, sent.error_text());
        let bs = Target::Im {
            guid: g,
            sender: b"react-b".to_vec(),
        };
        let react = |c: &mut Client, emoji: &str| {
            let key = Key::Emoji(emoji.into());
            let r = c.request(&req::send(&bs, ADD, Some(&key), Some(b"react-b")).unwrap());
            assert!(!r.is_error(), "{}: {}", s.name, r.error_text());
            assert_eq!(r.chunk(tag::REACTION_COUNT), None, "{}: a count", s.name);
        };
        react(&mut a, THUMBS);
        let from_a = b"react-a".to_vec();
        assert_eq!(
            change(&mut b, &bs, None),
            (true, thumbs(), from_a.clone()),
            "{}",
            s.name
        );

        let uid = b.server_info().uid.expect("react-b's uid");
        drop(b);
        loop {
            match a.next_event().unwrap_or_else(|e| panic!("{e}")) {
                Event::Session(hxsession::Event::UserLeft { uid: gone, .. }) if gone == uid => {
                    break
                }
                _ => {}
            }
        }
        let party = "\u{1F389}";
        react(&mut a, party);
        let mut b = messenger(s, "react-b", Handled::NONE);
        let roster = b.send(&im::roster());
        assert_eq!(
            change(&mut b, &bs, Some(roster)),
            (true, Key::Emoji(party.into()), from_a),
            "{}",
            s.name
        );
    }
}

/// `login` with reactions, its session handling private messages.
fn writer(s: &'static Server, login: &str) -> Client {
    let nick: String = unique_name(login).chars().take(31).collect();
    let c = Client::login_handling(s, login, Some(login), OFFER, Handled::MSG, &nick)
        .unwrap_or_else(|e| panic!("{e}"));
    let limits = c.server_info().reactions;
    assert!(
        limits.is_some_and(|l| l.targets & TARGETS_STORED_PMS != 0),
        "{login}: no reactions on stored messages: {limits:?}"
    );
    c
}

/// A private message the server stores is named by the same reference to
/// its sender, in the answer to the request that sent it, and to its
/// recipient, as it is handed over. A reaction to it is answered with no
/// count and its summary told to the other party; one made by somebody who
/// is no party to it is answered alike and dropped, and Get Reactions
/// answers each party the summary without it and that somebody none.
#[test]
fn a_stored_messages_reaction_reaches_its_other_party() {
    for s in servers_with(&[Cap::Reactions]) {
        let mut a = writer(s, "react-a");
        let mut b = writer(s, "react-b");
        let mut outsider = writer(s, "react-mod");
        let text = unique_name("react to this message");
        let to = b.server_info().uid.expect("react-b's uid");
        let message = Request {
            opcode: 108,
            chunks: vec![
                (tag::UID, to.to_be_bytes().to_vec()),
                (tag::BODY, text.as_bytes().to_vec()),
            ],
        };
        let t = a.send_expecting(&message, Some(Expect::Message));
        let sent = loop {
            match a.next_event().unwrap_or_else(|e| panic!("{e}")) {
                Event::Session(hxsession::Event::MessageSent { trans, pm_id }) if trans == t => {
                    break pm_id
                }
                Event::Session(hxsession::Event::Failed { trans, reason }) if trans == t => {
                    panic!("{}: refused: {reason:?}", s.name)
                }
                _ => {}
            }
        };
        let got = loop {
            match b.next_event().unwrap_or_else(|e| panic!("{e}")) {
                Event::Session(hxsession::Event::Message { text: t, pm_id, .. }) if t == text => {
                    break pm_id
                }
                _ => {}
            }
        };
        assert_eq!(got, sent, "{}: named otherwise to each", s.name);
        let Some((true, reference)) = sent else {
            panic!("{}: not named as stored: {sent:?}", s.name)
        };
        let target = Target::Pm {
            stored: true,
            reference,
        };

        let react = |c: &mut Client, emoji: &str| {
            let key = Key::Emoji(emoji.into());
            let r = c.request(&req::send(&target, ADD, Some(&key), None).unwrap());
            assert!(!r.is_error(), "{}: {}", s.name, r.error_text());
            assert_eq!(r.chunk(tag::REACTION_COUNT), None, "{}: a count", s.name);
        };
        react(&mut b, THUMBS);
        told(&mut a, &target, Some((summary(1), Mine::default())));
        react(&mut outsider, "\u{1F389}");

        let held = |c: &mut Client| {
            let r = c.request(&req::get(std::slice::from_ref(&target), None).unwrap());
            assert!(!r.is_error(), "{}: {}", s.name, r.error_text());
            let [g] = &parse_got(ChunkIter::over_message(&r.raw, r.raw.len()))[..] else {
                panic!("{}: not one group", s.name)
            };
            assert_eq!(g.target, target, "{}", s.name);
            (g.summary.clone(), g.mine.clone())
        };
        for (who, c, want) in [
            ("its sender", &mut a, (summary(1), Mine::default())),
            ("its recipient", &mut b, (summary(1), Mine(vec![0]))),
            ("no party", &mut outsider, Default::default()),
        ] {
            assert_eq!(held(c), want, "{}: {who}", s.name);
        }
    }
}
