//! Chat as production receives it: the session handles the chat domain
//! (`Handled::CHAT`) and hands its events on among the frames, which is
//! what `hxnet` does for GtkHx's main thread.
#![cfg(feature = "rig")]

use hx_e2e::client::CAP_TEXT_ENCODING;
use hx_e2e::{servers_with, unique_name, Cap, Client, Server};
use hxnet::Event;
use hxproto::messages::tag;
use hxrequest::Request;
use hxsession::{Expect, Handled, HistoryEntry};

/// `HTLC_CAP_CHAT_HISTORY`.
const CAP_CHAT_HISTORY: u16 = 0x0010;

/// A guest whose session handles chat, going by a name no other test uses.
fn chatter(server: &'static Server, who: &str) -> (Client, String) {
    chatter_over(server, who, None)
}

/// As [`chatter`], logged in over HOPE with `cipher` when there is one.
fn chatter_over(
    server: &'static Server,
    who: &str,
    cipher: Option<hxhope::Cipher>,
) -> (Client, String) {
    // Inside every server's 31-byte cap.
    let nick: String = unique_name(who).chars().take(31).collect();
    let caps = CAP_TEXT_ENCODING | CAP_CHAT_HISTORY;
    let mut c = match cipher {
        None => Client::login_handling(server, "", None, caps, Handled::CHAT, &nick),
        Some(cipher) => Client::hope_guest(server, cipher, caps, Handled::CHAT, &nick),
    }
    .unwrap_or_else(|e| panic!("{e}"));
    // The user list, as GtkHx asks for it once logged in: hlservd sends a
    // client no chat before then.
    c.request(&request(300, &[]));
    (c, nick)
}

fn request(opcode: u32, chunks: &[(u16, &[u8])]) -> Request {
    Request {
        opcode,
        chunks: chunks.iter().map(|(t, d)| (*t, d.to_vec())).collect(),
    }
}

fn chat(text: &[u8]) -> Request {
    request(105, &[(tag::BODY, text)])
}

/// A frame as the server sent it, header and all, as the parsers take it.
fn whole(f: &hxnet::Frame) -> Vec<u8> {
    let mut raw = vec![0u8; hxproto::HL_HDR_LEN];
    let h = &f.header;
    hxproto::build::pack_header(
        &mut raw,
        h.type_,
        h.trans,
        h.flag,
        h.hc,
        f.body.len() as u32,
    );
    raw.extend_from_slice(&f.body);
    raw
}

/// The uid `nick` has, from the user list.
fn uid_of(c: &mut Client, nick: &str) -> u16 {
    let reply = c.request(&request(300, &[]));
    hxproto::wire::ChunkIter::over_message(&reply.raw, reply.raw.len())
        .filter(|ch| ch.tag == tag::USER_LIST)
        .filter_map(|ch| hxproto::parse::parse_user_list_record(ch.data, 31))
        .find(|r| r.name == nick.as_bytes())
        .unwrap_or_else(|| panic!("{}: {nick} is not listed", c.server().name))
        .uid
}

/// What `c` is sent that `keep` has a name for, in order, until `done`
/// says that is everything.
fn heard<T>(
    c: &mut Client,
    mut keep: impl FnMut(&Event) -> Option<T>,
    done: impl Fn(&[T]) -> bool,
) -> Vec<T> {
    let mut out = Vec::new();
    while !done(&out) {
        let e = c.next_event().unwrap_or_else(|e| panic!("{e}"));
        out.extend(keep(&e));
    }
    out
}

#[test]
fn chat_arrives_as_events_in_the_order_the_server_sent_it() {
    for s in servers_with(&[]) {
        let (mut a, na) = chatter(s, "ca");
        let (mut b, nb) = chatter(s, "cb");
        let a_uid = uid_of(&mut b, &na);
        let b_uid = uid_of(&mut a, &nb);

        // A line, a change of icon others are told of as a frame, a line.
        let (one, two) = (format!("{na} one"), format!("{na} two"));
        a.send(&chat(one.as_bytes()));
        a.send(&request(
            304,
            &[(tag::NAME, na.as_bytes()), (tag::ICON, &7u16.to_be_bytes())],
        ));
        a.send(&chat(two.as_bytes()));
        let order = heard(
            &mut b,
            |e| match e {
                Event::Session(hxsession::Event::Chat { text, .. }) if text.contains(&na) => {
                    Some(text.rsplit(' ').next().unwrap_or_default().to_string())
                }
                Event::Frame(f) if f.header.type_ == 0x12d => {
                    let raw = whole(f);
                    let c = hxproto::parse::parse_user_change(&raw, raw.len(), 31);
                    (c.uid == a_uid && c.icon == 7).then(|| "frame".to_string())
                }
                _ => None,
            },
            |got| got.len() == 3,
        );
        assert_eq!(order, ["one", "frame", "two"], "{}", s.name);

        // A new chat with b invites b.
        a.send(&request(112, &[(tag::UID, &b_uid.to_be_bytes())]));
        heard(
            &mut b,
            |e| match e {
                Event::Session(hxsession::Event::ChatInvite { uid, name, .. })
                    if *uid == a_uid && *name == na =>
                {
                    Some(())
                }
                _ => None,
            },
            |got| !got.is_empty(),
        );
    }
}

/// Public chat's history as GtkHx asks for it, read as the session hands it
/// on: the entries, and whether there is more before them.
fn history(c: &mut Client, before: u64, after: u64, limit: u16) -> (Vec<HistoryEntry>, bool) {
    let name = c.server().name;
    let ask = hxrequest::chat::history(0, before, after, limit);
    let t = c.send_expecting(&ask, Some(Expect::ChatHistory { cid: 0 }));
    heard(
        c,
        |e| match e {
            Event::Session(hxsession::Event::ChatHistory {
                trans,
                entries,
                has_more,
                ..
            }) if *trans == t => Some((entries.clone(), *has_more)),
            Event::Session(hxsession::Event::Failed { trans, .. }) if *trans == t => {
                panic!("{name}: history refused: {e:?}")
            }
            Event::Frame(f) if f.header.trans == t => {
                panic!("{name}: the expected reply came whole")
            }
            _ => None,
        },
        |got| !got.is_empty(),
    )
    .remove(0)
}

/// The page of history `before` / `after` / `limit` asks for, once it holds
/// `line`: asked for again for a while, in case the server has not stored
/// the line yet when the first page is made.
fn page_with(c: &mut Client, line: &str, before: u64, after: u64, limit: u16) -> Vec<HistoryEntry> {
    for _ in 0..50 {
        let (entries, _) = history(c, before, after, limit);
        if entries.iter().any(|e| e.text.contains(line)) {
            return entries;
        }
        std::thread::sleep(std::time::Duration::from_millis(100));
    }
    panic!("{}: {line:?} never reached the history", c.server().name)
}

/// A line comes back in the history as its sender's, over plaintext and
/// over HOPE with each cipher, which seals the request, the line and the
/// reply alike.
#[test]
fn history_holds_what_was_said_over_every_transport() {
    for cipher in [
        None,
        Some(hxhope::Cipher::ChaCha20Poly1305),
        Some(hxhope::Cipher::Blowfish),
    ] {
        let mut caps = vec![Cap::ChatHistory];
        caps.extend(cipher.map(Cap::Hope));
        for s in servers_with(&caps) {
            let (mut a, na) = chatter_over(s, "ch", cipher);
            assert_ne!(a.caps() & CAP_CHAT_HISTORY, 0, "{} {cipher:?}", s.name);
            let line = format!("{na} for the record");
            a.send(&chat(line.as_bytes()));
            let entries = page_with(&mut a, &line, 0, 0, 50);
            let mine = entries.iter().find(|e| e.text.contains(&line)).unwrap();
            assert_eq!(mine.nick, na, "{} {cipher:?}", s.name);
            assert!(mine.message_id > 0, "{} {cipher:?}: {mine:?}", s.name);
        }
    }
}

/// A page holds no more than its limit and says when there is more; the
/// page before it holds only older lines, and a catch-up after it only
/// newer ones, a line said since among them.
#[test]
fn history_pages_by_limit_and_cursor() {
    for s in servers_with(&[Cap::ChatHistory]) {
        let (mut a, na) = chatter(s, "hp");
        for i in 0..6 {
            a.send(&chat(format!("{na} pad {i}").as_bytes()));
        }
        page_with(&mut a, &format!("{na} pad 5"), 0, 0, 50);

        let (page, has_more) = history(&mut a, 0, 0, 2);
        assert!(!page.is_empty() && page.len() <= 2, "{}: {page:?}", s.name);
        assert!(has_more, "{}", s.name);
        let oldest = page.iter().map(|e| e.message_id).min().unwrap();
        let newest = page.iter().map(|e| e.message_id).max().unwrap();

        let (before, _) = history(&mut a, oldest, 0, 10);
        assert!(
            !before.is_empty(),
            "{}: has_more, but nothing before",
            s.name
        );
        assert!(
            before.iter().all(|e| e.message_id < oldest),
            "{}: before {oldest}: {before:?}",
            s.name
        );

        let line = format!("{na} since");
        a.send(&chat(line.as_bytes()));
        let after = page_with(&mut a, &line, 0, newest, 50);
        assert!(
            after.iter().all(|e| e.message_id > newest),
            "{}: after {newest}: {after:?}",
            s.name
        );
    }
}

/// An emoji goes to a server that agreed to no text encoding as its
/// shortcode, which it relays as it is, and the view shows the emoji.
#[test]
fn an_emoji_crosses_a_server_that_knows_only_mac_roman() {
    for s in servers_with(&[]) {
        let (mut a, na) = chatter(s, "ea");
        let (mut b, _) = chatter(s, "eb");
        let typed = format!("{na} party time 🎉");
        let wire = hxtext::for_wire(typed.as_bytes(), false, true);
        assert!(wire.is_ascii(), "{}: {wire:?}", s.name);
        a.send(&chat(&wire));
        let text = heard(
            &mut b,
            |e| match e {
                Event::Session(hxsession::Event::Chat { text, .. }) if text.contains(&na) => {
                    Some(text.clone())
                }
                _ => None,
            },
            |got| !got.is_empty(),
        )
        .remove(0);
        assert!(text.contains(":tada:"), "{}: {text:?}", s.name);
        let ev = gtkhx_core::boxed::chat::chat_event_new(0, 0, &text, None, b"", true);
        // SAFETY: a fresh event, freed here.
        let line = unsafe {
            let e = &*ev;
            let line = std::slice::from_raw_parts(e.line as *const u8, e.line_len).to_vec();
            gtkhx_core::boxed::chat::hx_chat_event_free(ev);
            String::from_utf8(line).unwrap()
        };
        assert!(
            line.contains("party time 🎉") && !line.contains(":tada:"),
            "{}: {line:?}",
            s.name
        );
    }
}

/// A line far past a single-byte length arrives whole.
#[test]
fn a_long_line_arrives_whole() {
    for s in servers_with(&[]) {
        let (mut a, na) = chatter(s, "la");
        let (mut b, _) = chatter(s, "lb");
        let long: String = ('A'..='Z').chain('a'..='z').cycle().take(1500).collect();
        let line = format!("{na} {long}");
        a.send(&chat(line.as_bytes()));
        let text = heard(
            &mut b,
            |e| match e {
                Event::Session(hxsession::Event::Chat { text, .. }) if text.contains(&na) => {
                    Some(text.clone())
                }
                _ => None,
            },
            |got| !got.is_empty(),
        )
        .remove(0);
        assert!(text.contains(&line), "{}: {} bytes", s.name, text.len());
    }
}

/// Accented text crosses as UTF-8 where the server agreed to the text
/// encoding and as Mac Roman where it didn't, and reads back as typed.
#[test]
fn accented_text_crosses_in_the_server_s_encoding() {
    for s in servers_with(&[]) {
        let (mut a, na) = chatter(s, "ta");
        let (mut b, _) = chatter(s, "tb");
        let typed = if a.utf8() {
            format!("{na} café ☃ 日本語")
        } else {
            format!("{na} café naïve")
        };
        a.send(&chat(&hxtext::for_wire(typed.as_bytes(), a.utf8(), true)));
        let text = heard(
            &mut b,
            |e| match e {
                Event::Session(hxsession::Event::Chat { text, .. }) if text.contains(&na) => {
                    Some(text.clone())
                }
                _ => None,
            },
            |got| !got.is_empty(),
        )
        .remove(0);
        assert!(text.contains(&typed), "{}: {text:?}", s.name);
    }
}

/// The server relays a line as its sender's: the uid the ignore list is
/// checked against, and the sender's name ahead of the text.
#[test]
fn a_line_arrives_as_its_sender_s() {
    for s in servers_with(&[]) {
        let (mut a, na) = chatter(s, "sa");
        let (mut b, _) = chatter(s, "sb");
        let a_uid = uid_of(&mut b, &na);
        let body = unique_name("body");
        a.send(&chat(body.as_bytes()));
        let (uid, text) = heard(
            &mut b,
            |e| match e {
                Event::Session(hxsession::Event::Chat { uid, text, .. })
                    if text.contains(&body) =>
                {
                    Some((*uid, text.clone()))
                }
                _ => None,
            },
            |got| !got.is_empty(),
        )
        .remove(0);
        // Janus and hlservd stamp a relayed line with uid 0, so a line
        // from someone ignored there can only be told by its name.
        let stamped = !matches!(s.name, "janus" | "hlservd");
        assert_eq!(uid, if stamped { a_uid } else { 0 }, "{}", s.name);
        // mhxd and hlservd cut the name to the classic 13 columns; Janus
        // writes it whole.
        let shown: String = na.chars().take(13).collect();
        assert!(text.contains(&shown), "{}: {text:?}", s.name);
    }
}
