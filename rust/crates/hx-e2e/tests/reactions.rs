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
use hxproto::reactions::{
    parse_changed, parse_got, Group, Key, Mine, Summary, ADD, INVALID_KEY, REMOVE, TARGETS_LINES,
};
use hxproto::structured_chat::NOT_ALLOWED;
use hxproto::target::Target;
use hxproto::wire::ChunkIter;
use hxrequest::reactions as req;
use hxrequest::Request;
use hxsession::cap::{CHAT_HISTORY, REACTIONS, STRUCTURED_CHAT};
use hxsession::{Expect, Handled, HistoryEntry};

const OFFER: u64 = CAP_TEXT_ENCODING | CHAT_HISTORY | STRUCTURED_CHAT | REACTIONS;
const THUMBS: &str = "\u{1F44D}";

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
    c.send(&Request {
        opcode: 105,
        chunks: vec![(tag::BODY, text.as_bytes().to_vec())],
    });
    loop {
        if let Event::Session(hxsession::Event::Chat { line: Some(e), .. }) =
            c.next_event().unwrap_or_else(|e| panic!("{e}"))
        {
            if e.text == text {
                assert_eq!(e.flags & 0x0008, 0, "an id the history did not give");
                return Target::Line {
                    channel: 0,
                    id: e.message_id,
                };
            }
        }
    }
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
/// skipping what else it is told; a Reaction Changed arriving whole, as a
/// frame, fails the test.
fn told(c: &mut Client, target: &Target, want: (Summary, Mine)) {
    loop {
        match c.next_event().unwrap_or_else(|e| panic!("no 711: {e}")) {
            Event::Session(hxsession::Event::Unhandled { opcode: 711, frame }) => {
                for g in parse_changed(ChunkIter::over_message(&frame, frame.len())) {
                    match g {
                        Group::Summary {
                            target: t,
                            summary,
                            mine,
                        } if t == *target && (&summary, &mine) == (&want.0, &want.1) => return,
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
        told(&mut b, &line, (summary(1), Mine::default()));
        assert_eq!(react(&mut b, &line, ADD), 2, "{}", s.name);
        told(&mut a, &line, (summary(2), Mine(vec![0])));

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
        told(&mut b, &line, (summary(1), Mine(vec![0])));
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
