//! GIF icons (the fogWraith extension) as production asks for them: each
//! reply expected by the session as `hxhandlers` expects it, and another
//! user's change of icon heard as the frame it arrives as.
#![cfg(feature = "rig")]

use hx_e2e::{servers_with, unique_name, Cap, Client, Server};
use hxnet::Event;
use hxproto::wire::ChunkIter;
use hxrequest::{icon, Request};
use hxsession::{Expect, Handled};

/// `HTLS_HDR_ICON_CHANGE`.
const ICON_CHANGE: u32 = 0x748;

/// A 1x1 GIF89a: the signature servers check for, and a picture.
const TINY_GIF: &[u8] = &[
    0x47, 0x49, 0x46, 0x38, 0x39, 0x61, 0x01, 0x00, 0x01, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xff, 0xff, 0xff, 0x21, 0xf9, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2c, 0x00, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x01, 0x00, 0x00, 0x02, 0x02, 0x44, 0x01, 0x00, 0x3b,
];

/// A guest going by a name no other test uses, and its uid.
fn guest(server: &'static Server, who: &str) -> (Client, u16) {
    // Inside every server's 31-byte cap.
    let nick: String = unique_name(who).chars().take(31).collect();
    let mut c = Client::login_handling(server, "", None, 0, Handled::NONE, &nick)
        .unwrap_or_else(|e| panic!("{e}"));
    let t = c.send_expecting(
        &Request {
            opcode: 300,
            chunks: Vec::new(),
        },
        Some(Expect::UserList),
    );
    let users = until(&mut c, |e| match e {
        Event::Session(hxsession::Event::UserList { trans, users, .. }) if *trans == t => {
            Some(users.clone())
        }
        _ => None,
    });
    let uid = users
        .iter()
        .find(|u| u.name == nick)
        .unwrap_or_else(|| panic!("{}: {nick} is not listed", server.name))
        .uid;
    (c, uid)
}

/// What `c` is sent until `want` makes something of it. Every request here
/// is one the server should grant, so a refusal fails the test.
fn until<T>(c: &mut Client, mut want: impl FnMut(&Event) -> Option<T>) -> T {
    loop {
        let e = c.next_event().unwrap_or_else(|e| panic!("{e}"));
        if let Event::Session(hxsession::Event::Failed { trans, reason }) = &e {
            panic!("{}: trans {trans} refused: {reason:?}", c.server().name);
        }
        if let Some(t) = want(&e) {
            return t;
        }
    }
}

fn set(c: &mut Client, gif: &[u8]) {
    c.send_expecting(&icon::set(gif).unwrap(), Some(Expect::IconSet));
}

fn get(c: &mut Client, uid: u16) -> hxsession::Icon {
    let t = c.send_expecting(&icon::get(uid).unwrap(), Some(Expect::Icon));
    until(c, |e| match e {
        Event::Session(hxsession::Event::Icon { trans, icon }) if *trans == t => Some(icon.clone()),
        _ => None,
    })
}

#[test]
fn an_icon_set_reads_back_and_is_listed() {
    for s in servers_with(&[Cap::GifIcons]) {
        let (mut c, uid) = guest(s, "is");
        set(&mut c, TINY_GIF);
        let got = get(&mut c, uid);
        assert_eq!((got.uid, got.gif.as_slice()), (uid, TINY_GIF), "{}", s.name);

        let t = c.send_expecting(&icon::list(), Some(Expect::IconList));
        let icons = until(&mut c, |e| match e {
            Event::Session(hxsession::Event::IconList { trans, icons }) if *trans == t => {
                Some(icons.clone())
            }
            _ => None,
        });
        // Other tests' users are listed too.
        let ours = icons.iter().find(|i| i.uid == uid);
        assert_eq!(
            ours.map(|i| i.gif.as_slice()),
            Some(TINY_GIF),
            "{}: {icons:?}",
            s.name
        );
    }
}

#[test]
fn others_hear_of_a_new_icon() {
    for s in servers_with(&[Cap::GifIcons]) {
        let (mut a, a_uid) = guest(s, "ia");
        let (mut b, _) = guest(s, "ib");
        set(&mut a, TINY_GIF);
        // Read back, so a refused set fails here rather than as silence.
        get(&mut a, a_uid);
        until(&mut b, |e| match e {
            Event::Frame(f) if f.header.type_ == ICON_CHANGE => {
                let uid = hxproto::gif_icons::parse_icon_change(ChunkIter::at(&f.body, 0));
                (uid == Some(a_uid)).then_some(())
            }
            _ => None,
        });
    }
}

#[test]
fn an_empty_icon_clears_it() {
    for s in servers_with(&[Cap::GifIcons]) {
        let (mut c, uid) = guest(s, "ic");
        set(&mut c, TINY_GIF);
        assert_eq!(get(&mut c, uid).gif, TINY_GIF, "{}", s.name);
        set(&mut c, &[]);
        // Janus leaves the GIF out; a server may send it empty.
        let got = get(&mut c, uid);
        assert_eq!((got.uid, got.gif.len()), (uid, 0), "{}", s.name);
    }
}
