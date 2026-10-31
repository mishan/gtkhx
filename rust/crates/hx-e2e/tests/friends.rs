//! The messaging extension's friend family (capability bit 6) as GtkHx
//! speaks it: `hxrequest::messaging`'s requests, and the pushes and replies
//! as hxnet hands them to production's receive handlers, the session's own
//! `Unhandled` and `Reply`, read with hxproto's parsers.
//!
//! The rig's `friend-*` accounts keep what they decide between runs, so
//! each test has accounts of its own and starts by undoing what an earlier
//! run left: whatever it was, a removal and an unblock end it.
#![cfg(feature = "rig")]

use hx_e2e::client::CAP_TEXT_ENCODING;
use hx_e2e::{servers_with, Cap, Client, Reply, Server};
use hxnet::Event;
use hxproto::messages::tag;
use hxproto::messaging::{
    parse_entry, parse_roster, AckType, PresenceState, ReasonCode, RosterState, CAP_MESSAGING,
};
use hxrequest::messaging as req;

const OFFER: u16 = CAP_MESSAGING as u16 | CAP_TEXT_ENCODING;

/// The rig's hxd-ng, unless `GTKHX_TEST_SERVERS` leaves it out.
fn server() -> Option<&'static Server> {
    servers_with(&[Cap::Messaging]).first().copied()
}

/// `login` (its password is its name), offering the friend family.
fn messenger(server: &'static Server, login: &str) -> Client {
    let c = Client::login(server, login, Some(login), OFFER).unwrap_or_else(|e| panic!("{e}"));
    assert_ne!(c.caps() & CAP_MESSAGING, 0, "{login}: bit 6 not confirmed");
    c
}

fn ok(c: &mut Client, r: Option<hxrequest::Request>) -> Reply {
    let reply = c.request(&r.expect("a request the wire carries"));
    assert!(!reply.is_error(), "{}", reply.error_text());
    reply
}

fn reason(r: &Reply) -> Option<ReasonCode> {
    let c = r.chunk(tag::REASON_CODE)?;
    let v = c.iter().fold(0u32, |n, &b| (n << 8) | u32::from(b));
    ReasonCode::from_wire(u16::try_from(v).ok()?)
}

/// What the server pushed of `opcode` that `want` takes, skipping the rest;
/// one arriving whole, as a frame, fails the test.
fn pushed(c: &mut Client, opcode: u32, want: impl Fn(&[u8]) -> bool) -> Vec<u8> {
    loop {
        match c
            .next_event()
            .unwrap_or_else(|e| panic!("no {opcode}: {e}"))
        {
            Event::Session(hxsession::Event::Unhandled { opcode: o, frame })
                if o == opcode && want(&frame) =>
            {
                return frame
            }
            Event::Frame(f) if (800..=834).contains(&f.header.type_) => {
                panic!("{} came as a frame", f.header.type_)
            }
            _ => {}
        }
    }
}

/// An entry push about `login` in which `want` holds.
fn told(
    c: &mut Client,
    opcode: u32,
    login: &str,
    want: impl Fn(&hxproto::messaging::RosterEntry<'_>) -> bool,
) -> Vec<u8> {
    pushed(c, opcode, |f| {
        parse_entry(f, f.len()).is_ok_and(|e| e.login == login.as_bytes() && want(&e))
    })
}

fn field(frame: &[u8], t: u16) -> Option<Vec<u8>> {
    hxproto::wire::ChunkIter::over_message(frame, frame.len())
        .find(|c| c.tag == t)
        .map(|c| c.data.to_vec())
}

/// Nothing between `a` and `b`, either way, whatever an earlier run left.
fn strangers(a: &mut Client, a_login: &str, b: &mut Client, b_login: &str) {
    for (c, other) in [(&mut *a, b_login), (&mut *b, a_login)] {
        for r in [
            req::unblock(other.as_bytes()),
            req::remove_friend(other.as_bytes()),
        ] {
            // A removal of nothing is refused, which is as good.
            c.request(&r.unwrap());
        }
    }
}

/// `a` and `b` made friends, `a` asking.
fn befriend(a: &mut Client, a_login: &str, b: &mut Client, b_login: &str) {
    strangers(a, a_login, b, b_login);
    let asked = ok(
        a,
        req::add_friend(b_login.as_bytes(), Some(b"hi from GtkHx"), true),
    );
    assert_eq!(
        asked.chunk(tag::ROSTER_STATE),
        Some(&RosterState::PendingOut.wire().to_be_bytes()[..])
    );
    let request = told(b, 804, a_login, |_| true);
    let e = parse_entry(&request, request.len()).unwrap();
    assert_eq!(e.note, Some(&b"hi from GtkHx"[..]));
    ok(b, req::respond(a_login.as_bytes(), true));
    told(a, 801, b_login, |e| {
        e.roster_state() == Some(RosterState::Accepted)
    });
}

#[test]
fn two_accounts_become_friends_and_see_each_others_presence() {
    let Some(s) = server() else { return };
    let mut a = messenger(s, "friend-a");
    let mut b = messenger(s, "friend-b");
    befriend(&mut a, "friend-a", &mut b, "friend-b");

    let roster = ok(&mut b, Some(req::roster()));
    let listed = parse_roster(&roster.raw, roster.raw.len()).unwrap();
    let a_row = listed
        .entries()
        .find(|e| e.login == b"friend-a")
        .expect("friend-a on friend-b's roster");
    assert_eq!(a_row.roster_state(), Some(RosterState::Accepted));

    // The account keeps its presence between runs: a status no earlier
    // run set is a change.
    let status = hx_e2e::unique_name("lunch");
    let away = req::set_presence(PresenceState::Away, Some(status.as_bytes()), true);
    ok(&mut a, away);
    told(&mut b, 809, "friend-a", |e| {
        e.presence_state() == Some(PresenceState::Away) && e.status_text == Some(status.as_bytes())
    });

    // An alias, told whole.
    ok(&mut b, req::set_nickname(b"friend-a", b"Ay", true));
    told(&mut b, 801, "friend-a", |e| e.nickname == Some(&b"Ay"[..]));
}

#[test]
fn an_instant_message_carries_its_receipts_and_typing() {
    let Some(s) = server() else { return };
    let mut a = messenger(s, "friend-c");
    let mut b = messenger(s, "friend-d");
    befriend(&mut a, "friend-c", &mut b, "friend-d");

    // The server takes a message once by its id, and the rig lives for
    // days: one no earlier run sent.
    let mut guid = [0u8; 16];
    let now = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .unwrap()
        .as_nanos();
    guid.copy_from_slice(&now.to_be_bytes());
    let sent = ok(
        &mut a,
        req::im_send(b"friend-d", &guid, "lunch?\n\u{1f35c}".as_bytes(), true),
    );
    assert!(
        matches!(reason(&sent), None | Some(ReasonCode::Ok)),
        "{:?}",
        reason(&sent)
    );
    let m = pushed(&mut b, 811, |f| {
        field(f, tag::MESSAGE_GUID).as_deref() == Some(&guid[..])
    });
    assert_eq!(
        field(&m, tag::FRIEND_LOGIN).as_deref(),
        Some(&b"friend-c"[..])
    );
    assert_eq!(
        field(&m, tag::MESSAGE_BODY).as_deref(),
        Some("lunch?\n\u{1f35c}".as_bytes())
    );
    for ack in [AckType::Delivered, AckType::Read] {
        ok(&mut b, req::im_ack(&guid, ack, b"friend-c"));
        let receipt = pushed(&mut a, 812, |f| {
            field(f, tag::ACK_TYPE).as_deref() == Some(&ack.wire().to_be_bytes()[..])
        });
        assert_eq!(
            field(&receipt, tag::MESSAGE_GUID).as_deref(),
            Some(&guid[..])
        );
        assert_eq!(
            field(&receipt, tag::FRIEND_LOGIN).as_deref(),
            Some(&b"friend-d"[..])
        );
    }

    // Typing is never answered: it goes, and is heard.
    b.send(&req::im_typing(b"friend-c", true).unwrap());
    let typing = pushed(&mut a, 813, |_| true);
    assert_eq!(
        field(&typing, tag::FRIEND_LOGIN).as_deref(),
        Some(&b"friend-d"[..])
    );
    assert_eq!(
        field(&typing, tag::TYPING_STATE).as_deref(),
        Some(&[0, 1][..])
    );
}

#[test]
fn a_block_hides_the_blocker_until_it_is_lifted() {
    let Some(s) = server() else { return };
    let mut a = messenger(s, "friend-e");
    let mut b = messenger(s, "friend-f");
    befriend(&mut a, "friend-e", &mut b, "friend-f");

    ok(&mut a, req::block(b"friend-f"));
    // The friendship ends for the blocked side too.
    told(&mut b, 801, "friend-e", |e| {
        e.roster_state() == Some(RosterState::Removed)
    });
    let roster = ok(&mut a, Some(req::roster()));
    let listed = parse_roster(&roster.raw, roster.raw.len()).unwrap();
    let row = listed.entries().find(|e| e.login == b"friend-f").unwrap();
    assert_eq!(row.roster_state(), Some(RosterState::Blocked));

    // A blocker reads as an account nobody holds.
    let hidden = b.request(&req::find_user(b"friend-e").unwrap());
    assert!(hidden.is_error());
    assert_eq!(reason(&hidden), Some(ReasonCode::AccountNotFound));
    let refused = b.request(&req::im_send(b"friend-e", &[1; 16], b"hello?", true).unwrap());
    assert!(refused.is_error());

    ok(&mut a, req::unblock(b"friend-f"));
    let found = ok(&mut b, req::find_user(b"friend-e"));
    let listed = parse_roster(&found.raw, found.raw.len()).unwrap();
    assert_eq!(
        listed.entries().next().map(|e| e.login),
        Some(&b"friend-e"[..])
    );
}

#[test]
fn find_user_answers_for_an_exact_login_only() {
    let Some(s) = server() else { return };
    let mut c = messenger(s, "friend-g");
    let found = ok(&mut c, req::find_user(b"friend-a"));
    let listed = parse_roster(&found.raw, found.raw.len()).unwrap();
    assert_eq!(
        listed.entries().next().map(|e| e.login),
        Some(&b"friend-a"[..])
    );
    for missing in [&b"friend"[..], b"no-such-account"] {
        let r = c.request(&req::find_user(missing).unwrap());
        assert!(r.is_error(), "{missing:?}");
        assert_eq!(reason(&r), Some(ReasonCode::AccountNotFound), "{missing:?}");
    }
}

/// Offering bit 6 changes nothing for a server that never heard of it, or
/// for an account that takes no part, as a guest never does: the login
/// goes as it always did, and nothing of the extension is confirmed.
#[test]
fn a_guest_offering_bit_6_logs_in_everywhere_and_is_never_confirmed_it() {
    for s in servers_with(&[])
        .into_iter()
        .chain(servers_with(&[Cap::Messaging]))
    {
        let guest = Client::login(s, "", None, OFFER).unwrap_or_else(|e| panic!("{e}"));
        assert_eq!(guest.caps() & CAP_MESSAGING, 0, "{}", s.name);
    }
}
