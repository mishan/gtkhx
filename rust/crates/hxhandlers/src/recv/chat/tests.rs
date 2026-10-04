//! What each chat event becomes, through the recording doubles for the model
//! and signal C ABIs.

use super::test_env::{self, Emitted};
use super::*;

const HTLC: *mut c_void = 0x1000 as *mut c_void;

fn said(cid: u32, uid: u16, line: &str, is_self: bool, media: bool) -> Emitted {
    Emitted::Chat {
        cid,
        uid,
        line: line.into(),
        is_self,
        media,
    }
}

#[test]
fn a_line_reaches_the_view_unless_its_sender_is_ignored() {
    let png = ChatMedia {
        id: b"h".to_vec(),
        mime: b"image/png".to_vec(),
        width: None,
        height: None,
        bytes: None,
    };
    // (ignored, own name, shortcodes, uid, text, media, what the view gets)
    let cases = [
        (
            false,
            "",
            true,
            5,
            "bob: hi",
            None,
            vec![said(0, 5, "bob: hi", false, false)],
        ),
        (true, "", true, 5, "bob: hi", None, vec![]),
        (
            true,
            "",
            true,
            0,
            "*** news",
            None,
            vec![said(0, 0, "*** news", false, false)],
        ),
        (
            false,
            "me",
            true,
            5,
            "me: hi",
            None,
            vec![said(0, 5, "me: hi", true, false)],
        ),
        (
            false,
            "",
            true,
            5,
            "bob: :tada:",
            None,
            vec![said(0, 5, "bob: 🎉", false, false)],
        ),
        (
            false,
            "",
            false,
            5,
            "bob: :tada:",
            None,
            vec![said(0, 5, "bob: :tada:", false, false)],
        ),
        (
            false,
            "",
            true,
            5,
            "bob: look",
            Some(&png),
            vec![said(0, 5, "bob: look", false, true)],
        ),
    ];
    for (ignored, own, shortcodes, uid, text, media, want) in cases {
        test_env::reset();
        test_env::IGNORE.with(|c| c.set(ignored));
        test_env::SHORTCODES.with(|c| c.set(shortcodes));
        test_env::OWN_NAME.with(|c| *c.borrow_mut() = own.into());
        unsafe { line(HTLC, 0, uid, text, media) };
        assert_eq!(test_env::emitted(), want, "{text:?}, ignored {ignored}");
    }
}

#[test]
fn an_invitation_reaches_the_view_unless_its_sender_is_ignored() {
    for (ignored, want) in [
        (false, vec![Emitted::Invitation(7, "René".into())]),
        (true, vec![]),
    ] {
        test_env::reset();
        test_env::IGNORE.with(|c| c.set(ignored));
        unsafe { invited(HTLC, 7, 42, "René") };
        assert_eq!(test_env::emitted(), want);
    }
}

#[test]
fn only_a_new_subject_is_news() {
    test_env::reset();
    unsafe { subject(HTLC, 4, "Plans") };
    assert_eq!(
        test_env::emitted(),
        [
            Emitted::Subject(4, "Plans".into()),
            Emitted::SubjectNotice(4, "Plans".into())
        ]
    );
    for again in ["Plans", ""] {
        unsafe { subject(HTLC, 4, again) };
        assert_eq!(test_env::emitted(), [], "{again:?}");
    }
}

#[test]
fn a_long_subject_is_cut_where_a_character_ends_and_is_news_once() {
    // 128 two-byte characters: 256 bytes, one more than the model holds.
    let long = "é".repeat(128);
    let fitted = "é".repeat(127);
    assert_eq!(fit_subject(&long), fitted);
    assert_eq!(fit_subject("a\0b"), "a");
    test_env::reset();
    for _ in 0..2 {
        unsafe { subject(HTLC, 4, &long) };
    }
    assert_eq!(
        test_env::emitted(),
        [
            Emitted::Subject(4, fitted.clone()),
            Emitted::SubjectNotice(4, fitted)
        ]
    );
}

fn entry(message_id: u64) -> HistoryEntry {
    HistoryEntry {
        message_id,
        timestamp: 0,
        flags: 0,
        icon: 0,
        nick: "a".into(),
        text: "hi".into(),
    }
}

#[test]
fn a_page_of_history_reaches_the_view_and_moves_the_cursor_forward_only() {
    test_env::reset();
    test_env::CURSOR.with(|c| c.set(20));
    unsafe { history(HTLC, 9, 3, &[entry(10), entry(25)], true) };
    unsafe { history(HTLC, 10, 3, &[entry(5)], false) };
    assert_eq!(
        test_env::emitted(),
        [
            Emitted::History {
                cid: 3,
                ids: vec![10, 25],
                has_more: true
            },
            Emitted::History {
                cid: 3,
                ids: vec![5],
                has_more: false
            },
        ]
    );
    assert_eq!(test_env::CURSOR.with(|c| c.get()), 25);
}

fn page(cid: u32, has_more: bool) -> Emitted {
    Emitted::History {
        cid,
        ids: vec![],
        has_more,
    }
}

#[test]
fn a_failed_history_request_ends_its_chats_wait_and_a_refusal_is_shown() {
    let refused = Some("Not here.");
    // (older, the reason, what the view gets)
    let cases = [
        (
            false,
            refused,
            vec![page(3, false), Emitted::RequestFailed("Not here.".into())],
        ),
        // A refused "Load older" leaves more to load: its row stays.
        (
            true,
            refused,
            vec![page(3, true), Emitted::RequestFailed("Not here.".into())],
        ),
        (false, None, vec![page(3, false)]),
        (true, Some(CUT_SHORT), vec![page(3, true)]),
    ];
    for (older, reason, want) in cases {
        test_env::reset();
        history_requested(HTLC, 9, 3, older);
        unsafe { failed(HTLC, 9, reason) };
        assert_eq!(test_env::emitted(), want, "older {older}, {reason:?}");
        // Answered once.
        unsafe { failed(HTLC, 9, None) };
        assert_eq!(test_env::emitted(), []);
    }
    // A refusal of anything else is only shown.
    unsafe { failed(HTLC, 11, refused) };
    assert_eq!(
        test_env::emitted(),
        [Emitted::RequestFailed("Not here.".into())]
    );
}

#[test]
fn a_new_connection_forgets_what_the_last_one_asked() {
    test_env::reset();
    history_requested(HTLC, 9, 3, true);
    history_forget(HTLC);
    unsafe { failed(HTLC, 9, None) };
    assert_eq!(test_env::emitted(), []);
}

/// The session's own words for a reply cut short, which is no refusal.
#[test]
fn a_cut_short_reply_is_worded_as_the_session_words_it() {
    use hxsession::{Config, Event, Expect, Session};
    let mut s = Session::new(
        Config {
            raw: true,
            ..Config::guest("me")
        },
        0,
    );
    s.take_outgoing();
    s.feed(hxsession::SERVER_MAGIC, 0);
    let login = hxsession::request::Request::new(0x0001_0000)
        .pack(1)
        .unwrap();
    s.feed(&login, 0);
    let trans = s.take_trans();
    s.expect(trans, Expect::ChatInvite).unwrap();
    let frag = |total: u32| {
        let head = [0x0001_0000, trans, 0, total, 10]
            .map(u32::to_be_bytes)
            .concat();
        [head, vec![0; 10]].concat()
    };
    s.feed(&[frag(40), frag(41)].concat(), 0);
    let reason = std::iter::from_fn(|| s.poll_event()).find_map(|e| match e {
        Event::Failed { reason, .. } => reason,
        _ => None,
    });
    assert_eq!(reason.as_deref(), Some(CUT_SHORT));
}
