//! Messages as production receives them: the session handles the messages
//! domain (`Handled::MSG`) and hands its events on among the frames, and a
//! message's reply comes back as the session's, expected as `hxhandlers`
//! expects it.
#![cfg(feature = "rig")]

use std::time::{Duration, Instant};

use hx_e2e::client::CAP_TEXT_ENCODING;
use hx_e2e::{servers_with, unique_name, Client, Server};
use hxnet::Event;
use hxproto::messages::tag;
use hxrequest::Request;
use hxsession::{Expect, Handled};

/// A session that handles what the app's does, logged in as `login` (the
/// guest, or the server's admin), going by a name no other test uses.
fn member(server: &'static Server, login: &str, who: &str) -> (Client, String) {
    // Inside every server's 31-byte cap.
    let nick: String = unique_name(who).chars().take(31).collect();
    let c = Client::login_handling(
        server,
        login,
        None,
        CAP_TEXT_ENCODING,
        Handled::CHAT | Handled::USERS | Handled::MSG,
        &nick,
    )
    .unwrap_or_else(|e| panic!("{e}"));
    (c, nick)
}

fn request(opcode: u32, chunks: &[(u16, &[u8])]) -> Request {
    Request {
        opcode,
        chunks: chunks.iter().map(|(t, d)| (*t, d.to_vec())).collect(),
    }
}

/// What `c` is sent until `want` makes something of it; a message's frame
/// arriving whole fails the test.
fn until<T>(c: &mut Client, mut want: impl FnMut(&hxsession::Event) -> Option<T>) -> T {
    loop {
        match c.next_event().unwrap_or_else(|e| panic!("{e}")) {
            Event::Session(e) => {
                if let Some(t) = want(&e) {
                    return t;
                }
            }
            Event::Frame(f) if matches!(f.header.type_, 0x68 | 0x163 | 0x6f) => {
                panic!("{}: a message's frame came whole", c.server().name)
            }
            _ => {}
        }
    }
}

/// `nick`'s uid, from the user list `c` asks for, as the app asks for it:
/// another's login may still be on its way.
fn uid_of(c: &mut Client, nick: &str) -> u16 {
    loop {
        let t = c.send_expecting(&request(300, &[]), Some(Expect::UserList));
        let users = until(c, |e| match e {
            hxsession::Event::UserList { trans, users, .. } if *trans == t => Some(users.clone()),
            _ => None,
        });
        if let Some(u) = users.iter().find(|u| u.name == nick) {
            return u.uid;
        }
    }
}

/// Send `text` to `uid` as the app does, and the server's reason if it
/// refused: the user list asked for after it is answered after it, so
/// nothing by then means it worked.
fn message(c: &mut Client, uid: u16, text: &str) -> Option<String> {
    let t = c.send_expecting(
        &request(
            108,
            &[(tag::UID, &uid.to_be_bytes()), (tag::BODY, text.as_bytes())],
        ),
        Some(Expect::Message),
    );
    let list = c.send_expecting(&request(300, &[]), Some(Expect::UserList));
    until(c, |e| match e {
        hxsession::Event::Failed { trans, reason } if *trans == t => {
            Some(Some(reason.clone().unwrap_or_default()))
        }
        hxsession::Event::UserList { trans, .. } if *trans == list => Some(None),
        _ => None,
    })
}

#[test]
fn a_message_arrives_as_an_event() {
    for s in servers_with(&[]) {
        // hlservd's guest may not send messages; its admin may.
        let (mut a, na) = member(s, s.admin, "ma");
        let (mut b, nb) = member(s, "", "mb");
        let (a_uid, b_uid) = (uid_of(&mut b, &na), uid_of(&mut a, &nb));

        let text = format!("psst, {nb}");
        assert_eq!(message(&mut a, b_uid, &text), None, "{}", s.name);
        let (uid, from) = until(&mut b, |e| match e {
            hxsession::Event::Message {
                uid, from, text: t, ..
            } if *t == text => Some((*uid, from.clone())),
            _ => None,
        });
        assert_eq!((uid, from.as_str()), (a_uid, na.as_str()), "{}", s.name);
    }
}

#[test]
fn a_message_to_no_one_is_refused_or_unanswered() {
    for s in servers_with(&[]) {
        let (mut a, _) = member(s, "", "mr");
        let to_no_one = request(
            108,
            &[
                (tag::UID, &0xfff0u16.to_be_bytes()),
                (tag::BODY, b"anyone?"),
            ],
        );
        if s.name != "janus" {
            let t = a.send_expecting(&to_no_one, Some(Expect::Message));
            let reason = until(&mut a, |e| match e {
                hxsession::Event::Failed { trans, reason } if *trans == t => Some(reason.clone()),
                _ => None,
            });
            assert!(reason.is_some_and(|r| !r.is_empty()), "{}", s.name);
            continue;
        }
        // Janus sends no reply at all (docs/janus-bugs.md). Sent unexpected,
        // any reply would come back whole; the requests after it are
        // answered meanwhile.
        let t = a.send(&to_no_one);
        let deadline = Instant::now() + Duration::from_secs(3);
        while Instant::now() < deadline {
            let list = a.send_expecting(&request(300, &[]), Some(Expect::UserList));
            loop {
                match a.next_event().unwrap_or_else(|e| panic!("{e}")) {
                    Event::Frame(f) if hx_e2e::is_reply(&f, t) => panic!("janus answered: {f:?}"),
                    Event::Session(hxsession::Event::UserList { trans, .. }) if trans == list => {
                        break
                    }
                    _ => {}
                }
            }
            std::thread::sleep(Duration::from_millis(250));
        }
    }
}

#[test]
fn a_broadcast_arrives_as_an_event() {
    for s in servers_with(&[]) {
        let (mut a, na) = member(s, s.admin, "ba");
        let (mut b, _) = member(s, "", "bb");
        // As the app does once logged in: hlservd sends a client that has
        // not asked for the user list nothing of anyone else.
        uid_of(&mut b, &na);
        // As the app sends it, in the server's own encoding.
        let text = format!("{na} rebooting, café");
        a.send_expecting(
            &hxrequest::user::broadcast(text.as_bytes(), a.utf8()).unwrap(),
            Some(Expect::Message),
        );
        let from = until(&mut b, |e| match e {
            hxsession::Event::Broadcast { from, text: t, .. } if *t == text => Some(from.clone()),
            _ => None,
        });
        // Janus names no sender.
        let want = if s.name == "janus" { "" } else { na.as_str() };
        assert_eq!(from, want, "{}", s.name);
    }
}

/// mhxd sends a message to ourselves back, which GtkHx once got with no
/// sender's name; the pinned mhxd names us, and the stream stays in step.
#[test]
fn a_message_to_ourselves_comes_back_from_us() {
    for s in servers_with(&[]).into_iter().filter(|s| s.name == "mhxd") {
        let (mut a, na) = member(s, "", "me");
        let me = uid_of(&mut a, &na);
        let text = format!("note to {na}");
        let t = a.send_expecting(
            &request(
                108,
                &[(tag::UID, &me.to_be_bytes()), (tag::BODY, text.as_bytes())],
            ),
            Some(Expect::Message),
        );
        let list = a.send_expecting(&request(300, &[]), Some(Expect::UserList));
        let mut echo = None;
        until(&mut a, |e| match e {
            hxsession::Event::Message {
                uid, from, text: m, ..
            } if *m == text => {
                echo = Some((*uid, from.clone()));
                None
            }
            hxsession::Event::Failed { trans, reason } if *trans == t => {
                panic!("{}: refused: {reason:?}", s.name)
            }
            hxsession::Event::UserList { trans, .. } if *trans == list => Some(()),
            _ => None,
        });
        let echo = echo.unwrap_or_else(|| panic!("{}: no echo before the list", s.name));
        assert_eq!(echo, (me, na.clone()), "{}", s.name);
    }
}
