//! Users as production receives them: the session handles the users domain
//! (`Handled::USERS`) and hands its events on among the frames, and the
//! replies to the user list, a private chat's create and join, a user's
//! info, a kick and the account requests come back as the session's,
//! expected as `hxhandlers` expects them.
#![cfg(feature = "rig")]

use hx_e2e::client::CAP_TEXT_ENCODING;
use hx_e2e::{servers_with, unique_name, Cap, Client, Server};
use hxnet::Event;
use hxproto::messages::{tag, NICK_COLOR_NONE};
use hxrequest::{user, Request};
use hxsession::{Expect, Handled, User};
use std::time::{Duration, Instant};

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

/// How long a user may take to appear in a list, or leave it, and how
/// often to ask meanwhile.
const WAIT: Duration = Duration::from_secs(15);
const POLL: Duration = Duration::from_millis(100);

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
    let deadline = Instant::now() + WAIT;
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
        assert!(Instant::now() < deadline, "{nick} never listed");
        std::thread::sleep(POLL);
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

        // A line, a change of name and icon, a line.
        let (one, two) = (format!("{nb} one"), format!("{nb} two"));
        let renamed = nb.replacen("ub", "ur", 1);
        b.send(&request(105, &[(tag::BODY, one.as_bytes())]));
        b.send(&request(
            304,
            &[
                (tag::NAME, renamed.as_bytes()),
                (tag::ICON, &7u16.to_be_bytes()),
            ],
        ));
        b.send(&request(105, &[(tag::BODY, two.as_bytes())]));
        let order = heard(
            &mut a,
            |e| match e {
                hxsession::Event::Chat { text, .. } if text.contains(&nb) => {
                    Some(text.rsplit(' ').next().unwrap_or_default().to_string())
                }
                hxsession::Event::UserChanged { cid: 0, user }
                    if user.uid == b_uid && user.icon == 7 && user.name == renamed =>
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

/// A color cleared after one went reaches the others: a server keeps the
/// last color it was sent through a change that leaves it out.
#[test]
fn a_cleared_nick_color_reaches_the_others() {
    for s in servers_with(&[Cap::NickColors]) {
        let (mut a, na) = member(s, "ca");
        // A color is what opts a session in to the others'. Janus keeps red
        // for administrators, and no green this strong is red.
        a.send(&user::change(na.as_bytes(), a.utf8(), 414, 0x0000_8000, false).unwrap());
        let (mut b, nb) = member(s, "cb");
        let b_uid = listed(&mut a, &nb)
            .into_iter()
            .find(|u| u.name == nb)
            .unwrap()
            .uid;
        for (color, sent) in [(0x0012_8034, false), (NICK_COLOR_NONE, true)] {
            b.send(&user::change(nb.as_bytes(), b.utf8(), 414, color, sent).unwrap());
            until(
                &mut a,
                |e| matches!(e, hxsession::Event::UserChanged { cid: 0, user } if user.uid == b_uid && user.color == Some(color)),
            );
        }
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

        // a is told of b coming in; a line a sends to the chat reaches b
        // marked as the chat's; a is told of b going.
        until(
            &mut a,
            |e| matches!(e, hxsession::Event::UserChanged { cid: c, user } if *c == cid && user.uid == b_uid),
        );
        let line = format!("{na} in private");
        a.send(&request(
            105,
            &[(tag::BODY, line.as_bytes()), (tag::CHAT_ID, &id)],
        ));
        until(
            &mut b,
            |e| matches!(e, hxsession::Event::Chat { cid: c, text, .. } if *c == cid && text.contains(&line)),
        );
        b.send(&request(116, &[(tag::CHAT_ID, &id)]));
        until(
            &mut a,
            |e| matches!(e, hxsession::Event::UserLeft { cid: c, uid } if *c == cid && *uid == b_uid),
        );
    }
}

/// A session that handles what the app's does, logged in as `login` (the
/// guest, or the server's admin), going by a name no other test uses.
fn as_login(server: &'static Server, login: &str, who: &str) -> (Client, String) {
    let nick: String = unique_name(who).chars().take(31).collect();
    let c = Client::login_handling(
        server,
        login,
        None,
        CAP_TEXT_ENCODING,
        Handled::CHAT | Handled::USERS | Handled::MSG | Handled::NEWS | Handled::FILES,
        &nick,
    )
    .unwrap_or_else(|e| panic!("{e}"));
    (c, nick)
}

/// Send `req` as the app does, its reply expected as `what`, and wait for
/// what the session makes of the reply: `got` of the event, or the
/// server's reason for refusing.
fn ask<T>(
    c: &mut Client,
    req: &Request,
    what: Expect,
    got: impl Fn(&hxsession::Event) -> Option<T>,
) -> Result<T, String> {
    let t = c.send_expecting(req, Some(what));
    heard(
        c,
        |e| match e {
            hxsession::Event::Failed { trans, reason } if *trans == t => {
                Some(Err(reason.clone().unwrap_or_default()))
            }
            e => got(e).map(Ok),
        },
        |got| !got.is_empty(),
    )
    .remove(0)
}

/// Whether an account change worked, as its own answer says.
fn worked(c: &mut Client, req: &Request) -> Result<(), String> {
    ask(c, req, Expect::AccountChange, |e| {
        matches!(e, hxsession::Event::AccountChanged { .. }).then_some(())
    })
}

/// What the server said about `c`, during the login or after it.
fn self_info(c: &mut Client) -> hxsession::Event {
    let me = c
        .login_events()
        .iter()
        .find(|e| matches!(e, hxsession::Event::SelfInfo { .. }))
        .cloned();
    me.unwrap_or_else(|| {
        heard(
            c,
            |e| matches!(e, hxsession::Event::SelfInfo { .. }).then(|| e.clone()),
            |got| !got.is_empty(),
        )
        .remove(0)
    })
}

/// The server's word on us comes as an event, never a frame the app would
/// not know: our access bits always, our uid where the login reply did not
/// already say it.
#[test]
fn the_server_s_word_on_us_arrives_as_an_event() {
    for s in servers_with(&[]) {
        let (mut a, na) = member(s, "si");
        let me = self_info(&mut a);
        let hxsession::Event::SelfInfo { uid, access, .. } = me else {
            unreachable!()
        };
        let listed = listed(&mut a, &na);
        let a_uid = listed.iter().find(|u| u.name == na).unwrap().uid;
        assert!(access.is_some(), "{}: no access bits", s.name);
        assert!(uid.is_none_or(|u| u == a_uid), "{}: uid {uid:?}", s.name);
    }
}

/// A user's info, and a kick refused to a guest and done by an admin, as
/// the app asks for them.
#[test]
fn a_user_s_info_and_a_kick_come_back_as_events() {
    for s in servers_with(&[Cap::FileAdmin]) {
        let (mut a, na) = as_login(s, s.admin, "ka");
        let (mut b, nb) = as_login(s, "", "kb");
        let b_uid = listed(&mut a, &nb)
            .iter()
            .find(|u| u.name == nb)
            .unwrap()
            .uid;
        let a_uid = listed(&mut b, &na)
            .iter()
            .find(|u| u.name == na)
            .unwrap()
            .uid;

        let info = ask(
            &mut a,
            &hxrequest::user::info(b_uid).unwrap(),
            Expect::UserInfo,
            |e| match e {
                hxsession::Event::UserInfo { name, info, .. } => Some((name.clone(), info.clone())),
                _ => None,
            },
        );
        let (name, info) = info.unwrap_or_else(|e| panic!("{}: refused: {e}", s.name));
        assert_eq!(name, nb, "{}", s.name);
        assert!(info.contains("guest"), "{}: {info:?}", s.name);

        let refused = ask(
            &mut b,
            &hxrequest::user::kick(a_uid, false).unwrap(),
            Expect::Kick,
            |e| matches!(e, hxsession::Event::Kicked { .. }).then_some(()),
        );
        assert!(refused.is_err_and(|r| !r.is_empty()), "{}", s.name);

        let kicked = ask(
            &mut a,
            &hxrequest::user::kick(b_uid, false).unwrap(),
            Expect::Kick,
            |e| matches!(e, hxsession::Event::Kicked { .. }).then_some(()),
        );
        assert_eq!(kicked, Ok(()), "{}", s.name);
    }
}

/// An account made, read, saved and deleted as the user editor does it, its
/// name kept as the server's bytes: an admin makes one, reads it back,
/// changes it without its password, and deletes it.
#[test]
fn an_account_is_made_read_saved_and_deleted_as_the_editor_does_it() {
    for s in servers_with(&[Cap::FileAdmin]) {
        let (mut a, na) = as_login(s, s.admin, "aa");
        let hxsession::Event::SelfInfo {
            access: Some(me), ..
        } = self_info(&mut a)
        else {
            panic!("{}: no access bits", s.name)
        };
        let login = unique_name("acct").chars().take(31).collect::<String>();
        let login = login.as_bytes();
        let read = |a: &mut Client| {
            ask(
                a,
                &hxrequest::user::account_read(login).unwrap(),
                Expect::Account,
                |e| match e {
                    hxsession::Event::Account { account, .. } => Some(account.clone()),
                    _ => None,
                },
            )
        };
        // The bits as the wire carries them; 0x8E is Mac Roman é.
        let access = [0x60, 0x60, 0x0c, 0, 0, 0, 0, 0];
        // As the editor makes it: the login's read is refused, so it is
        // free.
        assert!(read(&mut a).is_err(), "{}: already there", s.name);
        let new = hxrequest::user::account_create(login, b"pw", b"Ren\x8e", access).unwrap();
        assert_eq!(worked(&mut a, &new), Ok(()), "{}: create", s.name);
        let got = read(&mut a).unwrap_or_else(|e| panic!("{}: read: {e}", s.name));
        assert_eq!(
            (&got.login[..], &got.name[..], got.access),
            (login, &b"Ren\x8e"[..], Some(u64::from_be_bytes(access))),
            "{}",
            s.name
        );

        let changed = hxrequest::user::account_save(login, b"", b"Ren", [0; 8]).unwrap();
        assert_eq!(worked(&mut a, &changed), Ok(()), "{}: change", s.name);
        let got = read(&mut a).unwrap_or_else(|e| panic!("{}: read: {e}", s.name));
        assert_eq!(
            (&got.name[..], got.access),
            (&b"Ren"[..], Some(0)),
            "{}",
            s.name
        );
        // The editor checks a new account's login before making it where
        // it may read accounts, which the admin may: the read just above
        // is that check for a login that is taken, and nothing is made.
        // HL_ACCESS_READ_USERS, bit 16: the third byte's top bit.
        assert_ne!(
            me.to_be_bytes()[2] & 0x80,
            0,
            "{}: may not read users",
            s.name
        );
        // Saved without a password, the account keeps the one it had.
        let login_str = std::str::from_utf8(login).unwrap();
        let nick: String = unique_name("au").chars().take(31).collect();
        Client::login_handling(s, login_str, Some("pw"), 0, Handled::NONE, &nick)
            .unwrap_or_else(|e| panic!("{e}"));
        // Deleted only once no one is logged in with it: mhxd can crash
        // deleting an account in use (docs/mhxd-bugs.md).
        let deadline = Instant::now() + WAIT;
        while listed(&mut a, &na).iter().any(|u| u.name == nick) {
            assert!(Instant::now() < deadline, "{}: {nick} never left", s.name);
            std::thread::sleep(POLL);
        }

        let delete = hxrequest::user::account_delete(login).unwrap();
        assert_eq!(worked(&mut a, &delete), Ok(()), "{}: delete", s.name);
        assert!(read(&mut a).is_err(), "{}: still there", s.name);
    }
}
