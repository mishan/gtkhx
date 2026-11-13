//! The Replies draft (capability bit 18) as GtkHx reads it: the target a
//! line answers, as the session reads it from the line's entry, live and
//! in a page of history, and the lines a page's replies answer, asked for
//! by id with `hxrequest::chat::history_ids` and read as the session reads
//! the answer. A reply on the classic wire is said with GtkHx's own
//! request, `hxrequest::chat::say`, and on the ng one by a scripted client.
#![cfg(feature = "rig")]

use hx_e2e::client::CAP_TEXT_ENCODING;
use hx_e2e::ng::Ng;
use hx_e2e::{servers_with, unique_name, Cap, Client, Server};
use hxnet::Event;
use hxproto::target::Target;
use hxrequest::Request;
use hxsession::cap::{CHAT_HISTORY, REPLIES, STRUCTURED_CHAT};
use hxsession::{Expect, Handled, HistoryEntry};
use serde_json::json;

const OFFER: u64 = CAP_TEXT_ENCODING | CHAT_HISTORY | STRUCTURED_CHAT | REPLIES;
/// The rig's hotline-ng port.
const NG: &str = "127.0.0.1:5525";
/// A history entry's flag: the line was taken away.
const DELETED: u16 = 0x0004;

/// `login` (its password is its name) offering `caps`, its session
/// handling chat.
fn chatter(s: &'static Server, login: &str, caps: u64) -> Client {
    let nick: String = unique_name(login).chars().take(31).collect();
    Client::login_handling(s, login, Some(login), caps, Handled::CHAT, &nick)
        .unwrap_or_else(|e| panic!("{e}"))
}

fn kept(id: u64) -> Target {
    Target::Line { channel: 0, id }
}

/// `login`, with replies to kept lines and to lines nobody keeps.
fn replier(s: &'static Server, login: &str) -> Client {
    let c = chatter(s, login, OFFER);
    let targets = c.server_info().replies;
    let notice = Target::Ephemeral { chat: 0, id: 1 };
    assert!(
        targets.is_some_and(|t| t.takes(&kept(1)) && t.takes(&notice)),
        "{login}: no replies to lines: {targets:?}"
    );
    c
}

/// Say `text` in the lobby, answering `to` if anything, as GtkHx does.
fn say(c: &mut Client, text: &str, to: Option<&Target>) {
    c.send(&hxrequest::chat::say(text.as_bytes(), 0, 0, None, to).unwrap());
}

/// The entry of the lobby line `text` that `c` hears.
fn heard(c: &mut Client, text: &str) -> HistoryEntry {
    loop {
        if let Event::Session(hxsession::Event::Chat {
            cid: 0,
            line: Some(e),
            ..
        }) = c.next_event().unwrap_or_else(|e| panic!("{e}"))
        {
            if e.text == text {
                return e;
            }
        }
    }
}

/// The answer to a Get Chat History for the lobby, as GtkHx expects it.
fn history_reply(c: &mut Client, ask: &Request) -> (Vec<HistoryEntry>, bool) {
    let t = c.send_expecting(ask, Some(Expect::ChatHistory { cid: 0 }));
    loop {
        match c.next_event().unwrap_or_else(|e| panic!("{e}")) {
            Event::Session(hxsession::Event::ChatHistory {
                trans,
                entries,
                has_more,
                ..
            }) if trans == t => return (entries, has_more),
            Event::Session(hxsession::Event::Failed { trans, reason, .. }) if trans == t => {
                panic!("history refused: {reason:?}")
            }
            _ => {}
        }
    }
}

/// Only where the draft is served is bit 18 confirmed, with the kinds it
/// keeps replies to beside it.
#[test]
fn bit_18_is_confirmed_only_where_it_is_served() {
    for s in servers_with(&[]) {
        let c = Client::guest(s, OFFER);
        assert_eq!(c.caps() & REPLIES, 0, "{}", s.name);
        assert_eq!(c.server_info().replies, None, "{}", s.name);
    }
    for s in servers_with(&[Cap::Replies]) {
        replier(s, "react-a");
    }
}

/// A reply said on the classic wire or the ng one reaches GtkHx's session
/// with the line it answers, live and in the history after it.
#[test]
fn a_reply_reaches_the_session_live_and_in_history() {
    for s in servers_with(&[Cap::Replies]) {
        let mut a = replier(s, "react-a");
        let mut b = replier(s, "react-b");
        let first = unique_name("answer this");
        say(&mut a, &first, None);
        let first = heard(&mut b, &first).message_id;
        let line = kept(first);

        let classic = unique_name("from classic");
        say(&mut a, &classic, Some(&line));
        // Heard before the ng line is said, which `heard` would pass over.
        let mut replies = vec![heard(&mut b, &classic), heard(&mut a, &classic)];
        // A server never looks a reply's line up, so one nobody keeps is
        // relayed by any id.
        let notice = Target::Ephemeral { chat: 0, id: 1 };
        let to_notice = unique_name("to a notice");
        say(&mut a, &to_notice, Some(&notice));
        for (who, c) in [("the sender", &mut a), ("another", &mut b)] {
            let e = heard(c, &to_notice);
            assert_eq!(e.reply_to.as_ref(), Some(&notice), "{}: {who}", s.name);
        }
        let mut ng = Ng::login_with(NG, "react-mod", "react-mod", json!({ "replies": true }))
            .unwrap_or_else(|e| panic!("{e}"));
        let from_ng = unique_name("from ng");
        ng.ok("chat", json!({ "text": from_ng, "re": { "line": first } }))
            .unwrap_or_else(|e| panic!("{}: {e}", s.name));
        replies.push(heard(&mut b, &from_ng));
        let mut ids = Vec::new();
        for e in replies {
            assert_eq!(e.reply_to.as_ref(), Some(&line), "{}: {}", s.name, e.text);
            ids.push(e.message_id);
        }

        let (page, _) = history_reply(&mut b, &hxrequest::chat::history(0, 0, first, 0));
        for id in ids {
            let e = page.iter().find(|e| e.message_id == id);
            let to = e.and_then(|e| e.reply_to.as_ref());
            assert_eq!(to, Some(&line), "{}: line {id} in the page", s.name);
        }
    }
}

/// The lines a page's replies answer, asked for by id with GtkHx's own
/// request: answered ascending whatever order they were asked in, a line
/// taken away as its tombstone, and one that does not exist left out.
#[test]
fn lines_asked_for_by_id_answer_a_pages_quotes() {
    for s in servers_with(&[Cap::Replies]) {
        let mut a = replier(s, "react-a");
        let mut b = replier(s, "react-b");
        let texts: Vec<String> = (0..3).map(|i| unique_name(&format!("quote {i}"))).collect();
        let mut ids = Vec::new();
        for text in &texts {
            say(&mut a, text, None);
            ids.push(heard(&mut b, text).message_id);
        }
        let mut moderator =
            Ng::login(NG, "react-mod", "react-mod").unwrap_or_else(|e| panic!("{e}"));
        moderator
            .ok("redact", json!({ "id": ids[1], "reason": "a test" }))
            .unwrap_or_else(|e| panic!("{}: {e}", s.name));

        let missing = ids[2] + 1_000_000;
        let ask = hxrequest::chat::history_ids(0, &[missing, ids[2], ids[1], ids[0]]).unwrap();
        let (got, has_more) = history_reply(&mut b, &ask);
        let got: Vec<_> = (got.iter())
            .map(|e| (e.message_id, e.flags & DELETED != 0, e.text.as_str()))
            .collect();
        let want = [
            (ids[0], false, texts[0].as_str()),
            (ids[1], true, ""),
            (ids[2], false, texts[2].as_str()),
        ];
        assert_eq!(got, want, "{}", s.name);
        assert!(!has_more, "{}", s.name);
    }
}
