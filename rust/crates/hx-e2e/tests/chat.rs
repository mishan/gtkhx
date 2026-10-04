//! Chat as production receives it: the session handles the chat domain
//! (`Handled::CHAT`) and hands its events on among the frames, which is
//! what `hxnet` does for GtkHx's main thread.
#![cfg(feature = "rig")]

use hx_e2e::client::CAP_TEXT_ENCODING;
use hx_e2e::{servers_with, unique_name, Cap, Client, Server};
use hxnet::Event;
use hxproto::messages::tag;
use hxrequest::Request;
use hxsession::{Expect, Handled};

/// `HTLC_CAP_CHAT_HISTORY`.
const CAP_CHAT_HISTORY: u16 = 0x0010;

/// A guest whose session handles chat, going by a name no other test uses.
fn chatter(server: &'static Server, who: &str) -> (Client, String) {
    // Inside every server's 31-byte cap.
    let nick: String = unique_name(who).chars().take(31).collect();
    let mut c = Client::login_handling(
        server,
        "",
        None,
        CAP_TEXT_ENCODING | CAP_CHAT_HISTORY,
        Handled::CHAT,
        &nick,
    )
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

#[test]
fn history_comes_back_as_an_event() {
    for s in servers_with(&[Cap::ChatHistory]) {
        let (mut a, na) = chatter(s, "ch");
        let line = format!("{na} for the record");
        a.send(&chat(line.as_bytes()));
        // The server's echo of the line: it is stored by then.
        heard(
            &mut a,
            |e| {
                matches!(e, Event::Session(hxsession::Event::Chat { text, .. }) if text.contains(&line))
                .then_some(())
            },
            |got| !got.is_empty(),
        );
        let limit = 50u16.to_be_bytes();
        let ask = request(
            700,
            &[
                (tag::CHANNEL_ID, &[0, 0, 0, 0]),
                (tag::HISTORY_LIMIT, &limit),
            ],
        );
        let t = a.send_expecting(&ask, Some(Expect::ChatHistory { cid: 0 }));
        let entries = heard(
            &mut a,
            |e| match e {
                Event::Session(hxsession::Event::ChatHistory { trans, entries, .. })
                    if *trans == t =>
                {
                    Some(entries.clone())
                }
                Event::Frame(f) if f.header.trans == t => {
                    panic!("{}: the expected reply came whole", s.name)
                }
                _ => None,
            },
            |got| !got.is_empty(),
        )
        .remove(0);
        assert!(
            entries.iter().any(|e| e.text.contains(&line)),
            "{}: {line:?} not in {entries:?}",
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
