//! The messaging extension's friend family (capability bit 6) as GtkHx
//! speaks it: `hxrequest::messaging`'s requests, and the pushes and replies
//! as hxnet hands them to production's receive handlers, the session's own
//! `Unhandled` and `Reply`, read with hxproto's parsers.
//!
//! The rig's `friend-*` accounts keep what they decide between runs, so
//! each test has accounts of its own and starts by undoing what an earlier
//! run left: whatever it was, a removal and an unblock end it. What a
//! message request leaves (a hold on its sender, the requests counted
//! against the pair) no request can undo, so the requests tests make
//! accounts of their own each run, through the rig's `requests-admin`.
#![cfg(feature = "rig")]

use hx_e2e::client::CAP_TEXT_ENCODING;
use hx_e2e::ng::Ng;
use hx_e2e::{servers_with, Cap, Client, Reply, Server};
use hxnet::Event;
use hxproto::messages::tag;
use hxproto::messaging::{
    parse_entry, parse_message_requests, parse_roster, AckType, PresenceState, ReasonCode,
    RequestAction, RosterState, CAP_MESSAGING,
};
use hxrequest::messaging as req;
use hxsession::Handled;
use serde_json::json;

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
    let away = req::set_presence(
        Some(PresenceState::Away),
        Some(status.as_bytes()),
        None,
        true,
    );
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

/// Accounts made for one test, taking part in messaging, each password
/// its login, and deleted when it ends.
struct Fresh {
    admin: Ng,
    made: Vec<String>,
}

impl Fresh {
    fn new() -> Fresh {
        let admin = Ng::login("127.0.0.1:5525", "requests-admin", "requests-admin")
            .unwrap_or_else(|e| panic!("{e}"));
        Fresh {
            admin,
            made: Vec::new(),
        }
    }

    fn account(&mut self, prefix: &str) -> String {
        let login = hx_e2e::unique_name(prefix);
        let access = ["read_chat", "send_chat", "send_msgs", "messaging"];
        let made = json!({ "login": login, "password": login, "access": access });
        self.admin
            .ok("account_create", made)
            .unwrap_or_else(|e| panic!("{e}"));
        self.made.push(login.clone());
        login
    }
}

impl Drop for Fresh {
    fn drop(&mut self) {
        for login in &self.made {
            if let Err(e) = self.admin.ok("account_delete", json!({ "login": login })) {
                eprintln!("{login} left on the rig: {e}");
            }
        }
    }
}

/// `login` logged in as GtkHx logs in where requests are served: its
/// requests listed before anything else, which makes it a session that
/// understands them.
fn requester(server: &'static Server, login: &str) -> Client {
    let mut c = messenger(server, login);
    let bound = c
        .server_info()
        .messaging
        .and_then(|l| l.max_request_messages);
    assert!(
        bound.is_some(),
        "{login}: no request bound in the login reply"
    );
    ok(&mut c, req::message_requests(None));
    c
}

/// `login` as a requester whose account takes requests, as it says itself.
fn taking(server: &'static Server, login: &str) -> Client {
    let mut c = requester(server, login);
    ok(
        &mut c,
        req::set_presence(Some(PresenceState::Online), None, Some(true), true),
    );
    c
}

/// A message id no earlier run sent.
fn guid() -> [u8; 16] {
    static N: std::sync::atomic::AtomicU8 = std::sync::atomic::AtomicU8::new(0);
    let now = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .unwrap()
        .as_nanos();
    let mut g = now.to_be_bytes();
    g[0] = N.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
    g
}

/// `body` to `to`: its id, and what the send was answered.
fn im(c: &mut Client, to: &str, body: &str) -> ([u8; 16], Reply) {
    let g = guid();
    let r = c.request(&req::im_send(to.as_bytes(), &g, body.as_bytes(), true).unwrap());
    (g, r)
}

fn sent_as(c: &mut Client, to: &str, body: &str, want: ReasonCode) -> [u8; 16] {
    let (g, r) = im(c, to, body);
    assert!(!r.is_error(), "{to}: {}", r.error_text());
    assert_eq!(reason(&r), Some(want), "{to}: {body}");
    g
}

/// What a listing's one page says: our setting, and each sender's Login
/// with its messages' bodies, then the approvals.
struct Listed {
    takes: Option<bool>,
    senders: Vec<(Vec<u8>, Vec<Vec<u8>>)>,
    approvals: Vec<Vec<u8>>,
}

fn listed(c: &mut Client) -> Listed {
    let r = ok(c, req::message_requests(None));
    let p = parse_message_requests(&r.raw, r.raw.len()).unwrap();
    assert_eq!(p.cursor, None, "a fresh account's listing is one page");
    let (mut senders, mut approvals) = (Vec::new(), Vec::new());
    for g in p.groups() {
        if g.is_approval() {
            approvals.push(g.login.to_vec());
        } else {
            let bodies = g.messages().map(|m| m.body.to_vec()).collect();
            senders.push((g.login.to_vec(), bodies));
        }
    }
    Listed {
        takes: p.takes_requests,
        senders,
        approvals,
    }
}

fn logins(l: &Listed) -> Vec<&[u8]> {
    l.senders.iter().map(|(l, _)| l.as_slice()).collect()
}

/// The instant message `guid` arriving at `c`.
fn arrives(c: &mut Client, guid: &[u8; 16]) -> Vec<u8> {
    pushed(c, 811, |f| {
        field(f, tag::MESSAGE_GUID).as_deref() == Some(&guid[..])
    })
}

#[test]
fn a_strangers_message_waits_as_a_request_until_approved_or_ended() {
    let Some(s) = server() else { return };
    let mut fresh = Fresh::new();
    let (r_login, s_login) = (fresh.account("rq-r"), fresh.account("rq-s"));
    let mut r = taking(s, &r_login);
    let mut sender = requester(s, &s_login);

    let first = sent_as(&mut sender, &r_login, "hello?", ReasonCode::Requested);
    let changed = pushed(&mut r, 832, |_| true);
    assert_eq!(
        field(&changed, tag::REQUEST_COUNT).as_deref(),
        Some(&[0, 0, 0, 1][..])
    );
    let waiting = listed(&mut r);
    assert_eq!(waiting.takes, Some(true));
    assert_eq!(
        waiting.senders,
        [(s_login.as_bytes().to_vec(), vec![b"hello?".to_vec()])]
    );

    // Approved, what waited is delivered, and what follows is answered
    // as queued, whoever is there to take it.
    ok(
        &mut r,
        req::answer_request(s_login.as_bytes(), RequestAction::Approve),
    );
    let m = arrives(&mut r, &first);
    assert_eq!(
        field(&m, tag::FRIEND_LOGIN).as_deref(),
        Some(s_login.as_bytes())
    );
    let later = sent_as(&mut sender, &r_login, "thanks", ReasonCode::OfflineQueued);
    arrives(&mut r, &later);
    let approved = listed(&mut r);
    assert_eq!(logins(&approved), Vec::<&[u8]>::new());
    assert_eq!(approved.approvals, [s_login.as_bytes()]);

    // Ended, the sender is held: answered as a request, kept nowhere.
    ok(
        &mut r,
        req::answer_request(s_login.as_bytes(), RequestAction::EndApproval),
    );
    sent_as(&mut sender, &r_login, "still there?", ReasonCode::Requested);
    let ended = listed(&mut r);
    assert!(ended.senders.is_empty() && ended.approvals.is_empty());
}

/// Get Roster hands over what waited under an approval only to a session
/// that has listed its requests, so GtkHx lists them first: a session
/// that reads the roster before listing them is handed nothing of it,
/// and once it has listed them, the next read hands it over.
#[test]
fn an_approvals_message_waiting_at_login_is_handed_over_at_get_roster() {
    let Some(s) = server() else { return };
    let mut fresh = Fresh::new();
    let (r_login, s_login) = (fresh.account("rq-r"), fresh.account("rq-s"));
    // The sender hears users leave, to know when the recipient has gone.
    let mut sender =
        Client::login_handling(s, &s_login, Some(&s_login), OFFER, Handled::USERS, "hx-e2e")
            .unwrap_or_else(|e| panic!("{e}"));
    ok(&mut sender, req::message_requests(None));
    let mut r = taking(s, &r_login);
    let r_uid = r
        .server_info()
        .uid
        .or_else(|| {
            r.login_events().iter().find_map(|e| match e {
                hxsession::Event::SelfInfo { uid, .. } => *uid,
                _ => None,
            })
        })
        .expect("the recipient's uid");
    let first = sent_as(&mut sender, &r_login, "hi", ReasonCode::Requested);
    ok(
        &mut r,
        req::answer_request(s_login.as_bytes(), RequestAction::Approve),
    );
    arrives(&mut r, &first);
    drop(r);
    loop {
        match sender.next_event().unwrap_or_else(|e| panic!("{e}")) {
            Event::Session(hxsession::Event::UserLeft { uid, .. }) if uid == r_uid => break,
            _ => {}
        }
    }

    let gone = sent_as(
        &mut sender,
        &r_login,
        "while away",
        ReasonCode::OfflineQueued,
    );
    let mut r = messenger(s, &r_login);
    ok(&mut r, Some(req::roster()));
    // What the roster read handed over comes before the next reply.
    let listing = r.send(&req::message_requests(None).unwrap());
    loop {
        match r.next_event().unwrap_or_else(|e| panic!("{e}")) {
            Event::Session(hxsession::Event::Reply { trans, .. }) if trans == listing => break,
            Event::Session(hxsession::Event::Unhandled { opcode: 811, frame }) => assert_ne!(
                field(&frame, tag::MESSAGE_GUID).as_deref(),
                Some(&gone[..]),
                "handed over before the requests were listed"
            ),
            _ => {}
        }
    }
    ok(&mut r, Some(req::roster()));
    let m = arrives(&mut r, &gone);
    assert_eq!(
        field(&m, tag::MESSAGE_BODY).as_deref(),
        Some(&b"while away"[..])
    );
}

#[test]
fn a_deleted_request_holds_its_sender_and_a_reply_approves() {
    let Some(s) = server() else { return };
    let mut fresh = Fresh::new();
    let r_login = fresh.account("rq-r");
    let (one, two) = (fresh.account("rq-s"), fresh.account("rq-s"));
    let mut r = taking(s, &r_login);
    let mut deleted = requester(s, &one);
    let mut answered = requester(s, &two);
    sent_as(&mut deleted, &r_login, "spam", ReasonCode::Requested);
    let asked = sent_as(&mut answered, &r_login, "lunch?", ReasonCode::Requested);
    assert_eq!(logins(&listed(&mut r)), [one.as_bytes(), two.as_bytes()]);

    ok(
        &mut r,
        req::answer_request(one.as_bytes(), RequestAction::Delete),
    );
    sent_as(&mut deleted, &r_login, "more spam", ReasonCode::Requested);

    // Writing back is consent: the reply goes, and what waited arrives.
    let (back, reply) = im(&mut r, &two, "sure");
    assert!(!reply.is_error(), "{}", reply.error_text());
    arrives(&mut answered, &back);
    arrives(&mut r, &asked);
    let after = listed(&mut r);
    assert!(after.senders.is_empty(), "{:?}", logins(&after));
    assert_eq!(after.approvals, [two.as_bytes()]);
}

#[test]
fn a_clearing_says_how_many_requests_and_friend_requests_went() {
    let Some(s) = server() else { return };
    let mut fresh = Fresh::new();
    let r_login = fresh.account("rq-r");
    let others: Vec<String> = (0..3).map(|_| fresh.account("rq-s")).collect();
    let mut r = taking(s, &r_login);
    for o in &others[..2] {
        let mut c = requester(s, o);
        sent_as(&mut c, &r_login, "hi", ReasonCode::Requested);
    }
    let mut asks = requester(s, &others[2]);
    ok(&mut asks, req::add_friend(r_login.as_bytes(), None, true));
    told(&mut r, 804, &others[2], |_| true);

    let cleared = ok(&mut r, Some(req::clear_requests(None, None)));
    let count = |t| field(&cleared.raw, t);
    assert_eq!(
        count(tag::REQUEST_COUNT).as_deref(),
        Some(&[0, 0, 0, 2][..])
    );
    assert_eq!(
        count(tag::FRIEND_REQUEST_COUNT).as_deref(),
        Some(&[0, 0, 0, 1][..])
    );
    assert!(listed(&mut r).senders.is_empty());
}

#[test]
fn an_account_that_takes_no_requests_answers_not_friends() {
    let Some(s) = server() else { return };
    let mut fresh = Fresh::new();
    let (r_login, s_login) = (fresh.account("rq-r"), fresh.account("rq-s"));
    let mut r = taking(s, &r_login);
    ok(
        &mut r,
        req::set_presence(Some(PresenceState::Online), None, Some(false), true),
    );
    assert_eq!(listed(&mut r).takes, Some(false));
    let mut sender = requester(s, &s_login);
    let (_, refused) = im(&mut sender, &r_login, "hello?");
    assert!(refused.is_error());
    assert_eq!(reason(&refused), Some(ReasonCode::NotFriends));
}
