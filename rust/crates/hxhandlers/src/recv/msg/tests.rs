//! What a private message, a broadcast and the server's parting words become,
//! driven through the `test_env` recording doubles.

use super::test_env::{self, Emitted};
use super::*;

/// A sentinel connection pointer (never dereferenced).
const HTLC: usize = 0xC0DE;

fn htlc() -> *mut c_void {
    HTLC as *mut c_void
}

fn msg(uid: u16, name: &str, body: &str, is_self: bool) -> Emitted {
    Emitted::Msg {
        htlc: HTLC,
        uid,
        name: name.into(),
        body: body.into(),
        is_self,
        media: None,
    }
}

/// The emit carries the connection: a uid is only unique within one, so a
/// message without it lands in another server's window.
#[test]
fn a_message_is_the_msg_signal_on_its_connection() {
    test_env::reset();
    test_env::OWN_NAME.with(|c| *c.borrow_mut() = "misha".into());
    unsafe {
        message(htlc(), 42, "alice", "hi :tada:", None);
        message(htlc(), 7, "misha", "echo", None);
    }
    assert_eq!(
        test_env::emitted(),
        [
            msg(42, "alice", "hi 🎉", false),
            msg(7, "misha", "echo", true)
        ]
    );
}

#[test]
fn a_message_s_picture_rides_on_the_signal() {
    test_env::reset();
    let png = hxsession::ChatMedia {
        id: vec![0xAB, 0xCD],
        mime: b"image/png".to_vec(),
        width: None,
        height: None,
        bytes: None,
    };
    unsafe { message(htlc(), 42, "alice", "[image]", Some(&png)) };
    let Emitted::Msg { media, .. } = &test_env::emitted()[0] else {
        panic!("no msg signal");
    };
    assert_eq!(media.as_deref(), Some(&[0xAB, 0xCD][..]));
}

#[test]
fn a_message_the_server_left_unnamed_is_named_for_its_sender() {
    test_env::reset();
    test_env::OWN_UID.with(|c| c.set(7));
    test_env::OWN_NAME.with(|c| *c.borrow_mut() = "misha".into());
    test_env::MEMBER.with(|c| *c.borrow_mut() = Some(("bob".into(), 0)));
    unsafe {
        message(htlc(), 7, "", "to myself", None);
        message(htlc(), 5, "", "from bob", None);
    }
    assert_eq!(
        test_env::emitted(),
        [
            msg(7, "misha", "to myself", true),
            msg(5, "bob", "from bob", false)
        ]
    );
}

#[test]
fn a_broadcast_carries_its_sender_and_the_sender_s_status() {
    test_env::reset();
    test_env::MEMBER.with(|c| *c.borrow_mut() = Some(("admin".into(), 2)));
    unsafe {
        broadcast(htlc(), 5, "admin", "Rebooting");
        test_env::MEMBER.with(|c| c.borrow_mut().take());
        broadcast(htlc(), 0, "", "Rebooting");
        parting(htlc(), "Bye.");
    }
    let b = |name: Option<&str>, status, text: &str, parting| Emitted::Broadcast {
        name: name.map(Into::into),
        status,
        text: text.into(),
        parting,
    };
    assert_eq!(
        test_env::emitted(),
        [
            b(Some("admin"), 2, "Rebooting", false),
            b(None, 0, "Rebooting", false),
            b(None, 0, "Bye.", true),
        ]
    );
}

#[test]
fn what_an_ignored_user_sends_is_dropped() {
    test_env::reset();
    test_env::IGNORE.with(|c| c.set(true));
    unsafe {
        message(htlc(), 42, "alice", "hi", None);
        broadcast(htlc(), 42, "alice", "hi");
    }
    assert_eq!(test_env::emitted(), []);
}
