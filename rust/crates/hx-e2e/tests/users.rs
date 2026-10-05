//! Users as production receives them: the session handles the users domain
//! (`Handled::USERS`) and hands its events on among the frames, and the
//! user list and a private chat's create and join replies come back as the
//! session's, expected as `hxhandlers` expects them.
#![cfg(feature = "rig")]

use hx_e2e::client::CAP_TEXT_ENCODING;
use hx_e2e::{servers_with, unique_name, Client, Server};
use hxnet::Event;
use hxproto::messages::tag;
use hxrequest::Request;
use hxsession::{Expect, Handled, User};

/// A guest whose session handles chat and users, as the app's does, going
/// by a name no other test uses.
fn member(server: &'static Server, who: &str) -> (Client, String) {
    // Inside every server's 31-byte cap.
    let nick: String = unique_name(who).chars().take(31).collect();
    let c = Client::login_handling(
        server,
        "",
        None,
        CAP_TEXT_ENCODING,
        Handled::CHAT | Handled::USERS,
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

/// What `c` is sent that `keep` has a name for, in order, until `done`
/// says that is everything.
fn heard<T>(
    c: &mut Client,
    mut keep: impl FnMut(&hxsession::Event) -> Option<T>,
    done: impl Fn(&[T]) -> bool,
) -> Vec<T> {
    let mut out = Vec::new();
    while !done(&out) {
        match c.next_event().unwrap_or_else(|e| panic!("{e}")) {
            Event::Session(e) => out.extend(keep(&e)),
            Event::Frame(f) if matches!(f.header.type_, 0x12d | 0x12e | 0x75 | 0x76) => {
                panic!("{}: a user's frame came whole", c.server().name)
            }
            _ => {}
        }
    }
    out
}

/// Wait for what `c` is sent until `want` matches it.
fn until(c: &mut Client, want: impl Fn(&hxsession::Event) -> bool) {
    heard(c, |e| want(e).then_some(()), |got| !got.is_empty());
}

/// The users list `c` asks for, as the app asks for it once logged in, once
/// it has `nick` in it: another's login may still be on its way.
fn listed(c: &mut Client, nick: &str) -> Vec<User> {
    loop {
        let t = c.send_expecting(&request(300, &[]), Some(Expect::UserList));
        let users = heard(
            c,
            |e| match e {
                hxsession::Event::UserList { trans, users, .. } if *trans == t => {
                    Some(users.clone())
                }
                hxsession::Event::Failed { trans, reason } if *trans == t => {
                    panic!("the user list was refused: {reason:?}")
                }
                _ => None,
            },
            |got| !got.is_empty(),
        )
        .remove(0);
        if users.iter().any(|u| u.name == nick) {
            return users;
        }
    }
}

#[test]
fn users_arrive_change_and_leave_as_events_in_the_server_s_order() {
    for s in servers_with(&[]) {
        let (mut a, na) = member(s, "ua");
        listed(&mut a, &na);
        let (mut b, nb) = member(s, "ub");
        let b_uid = listed(&mut b, &nb)
            .into_iter()
            .find(|u| u.name == nb)
            .unwrap()
            .uid;
        until(
            &mut a,
            |e| matches!(e, hxsession::Event::UserChanged { cid: 0, user } if user.uid == b_uid),
        );

        // A line, a change of icon, a line.
        let (one, two) = (format!("{nb} one"), format!("{nb} two"));
        b.send(&request(105, &[(tag::BODY, one.as_bytes())]));
        b.send(&request(
            304,
            &[(tag::NAME, nb.as_bytes()), (tag::ICON, &7u16.to_be_bytes())],
        ));
        b.send(&request(105, &[(tag::BODY, two.as_bytes())]));
        let order = heard(
            &mut a,
            |e| match e {
                hxsession::Event::Chat { text, .. } if text.contains(&nb) => {
                    Some(text.rsplit(' ').next().unwrap_or_default().to_string())
                }
                hxsession::Event::UserChanged { cid: 0, user }
                    if user.uid == b_uid && user.icon == 7 =>
                {
                    Some("change".to_string())
                }
                _ => None,
            },
            |got| got.len() == 3,
        );
        assert_eq!(order, ["one", "change", "two"], "{}", s.name);

        drop(b);
        until(
            &mut a,
            |e| matches!(e, hxsession::Event::UserLeft { cid: 0, uid } if *uid == b_uid),
        );
    }
}

#[test]
fn a_private_chat_s_create_and_join_come_back_as_events() {
    for s in servers_with(&[]) {
        let (mut a, na) = member(s, "pa");
        let (mut b, nb) = member(s, "pb");
        let b_uid = listed(&mut a, &nb)
            .into_iter()
            .find(|u| u.name == nb)
            .unwrap()
            .uid;

        let t = a.send_expecting(
            &request(112, &[(tag::UID, &b_uid.to_be_bytes())]),
            Some(Expect::ChatCreate),
        );
        let (cid, me) = heard(
            &mut a,
            |e| match e {
                hxsession::Event::ChatCreated { trans, cid, user } if *trans == t => {
                    Some((*cid, user.clone()))
                }
                _ => None,
            },
            |got| !got.is_empty(),
        )
        .remove(0);
        assert_ne!(cid, 0, "{}", s.name);
        assert_eq!(me.name, na, "{}", s.name);

        until(
            &mut b,
            |e| matches!(e, hxsession::Event::ChatInvite { cid: c, .. } if *c == cid),
        );
        let id = cid.to_be_bytes();
        let t = b.send_expecting(
            &request(115, &[(tag::CHAT_ID, &id)]),
            Some(Expect::ChatJoin { cid }),
        );
        let users = heard(
            &mut b,
            |e| match e {
                hxsession::Event::ChatJoined {
                    trans,
                    cid: c,
                    users,
                    ..
                } if *trans == t && *c == cid => Some(users.clone()),
                _ => None,
            },
            |got| !got.is_empty(),
        )
        .remove(0);
        assert!(
            users.iter().any(|u| u.name == na),
            "{}: {na} not in {users:?}",
            s.name
        );

        // a is told of b coming in, and going.
        until(
            &mut a,
            |e| matches!(e, hxsession::Event::UserChanged { cid: c, user } if *c == cid && user.uid == b_uid),
        );
        b.send(&request(116, &[(tag::CHAT_ID, &id)]));
        until(
            &mut a,
            |e| matches!(e, hxsession::Event::UserLeft { cid: c, uid } if *c == cid && *uid == b_uid),
        );
    }
}
