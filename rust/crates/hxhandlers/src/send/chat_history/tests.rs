//! What each history fetch puts on the wire. The native builder runs for
//! real; the cap check, the cursor and the write primitive are stubbed,
//! recording what was handed to `hlwrite_chunks`.

use super::*;
use std::cell::{Cell, RefCell};

use hxproto::messages::{tag, ClientHdr};

use crate::recv::chat::test_env::{self, Emitted};

/// A request's chunks: (tag, data).
type Fields = Vec<(u16, Vec<u8>)>;

thread_local! {
    static CAPS: Cell<u64> = const { Cell::new(0) };
    static NEWEST: Cell<u64> = const { Cell::new(0) };
    static SENT: RefCell<Option<Fields>> = const { RefCell::new(None) };
}

pub(crate) unsafe fn hx_conn_has_cap(_htlc: *const c_void, cap: u64) -> gboolean {
    if CAPS.with(|c| c.get()) & cap != 0 {
        GTRUE
    } else {
        GFALSE
    }
}

pub(crate) unsafe fn hx_conn_chat_history_last_msgid(_htlc: *const c_void) -> u64 {
    NEWEST.with(|c| c.get())
}

pub(crate) unsafe fn debug_log_str(
    _cat: *const std::os::raw::c_char,
    _msg: *const std::os::raw::c_char,
) {
}

pub(crate) unsafe fn hlwrite_chunks(
    _htlc: *mut c_void,
    ty: u32,
    _flag: u32,
    chunks: *const HxChunk,
    hc: c_int,
) {
    assert_eq!(ty, ClientHdr::GetChatHistory as u32);
    let fields = (0..hc as usize)
        .map(|i| {
            let c = &*chunks.add(i);
            (
                c.tag,
                std::slice::from_raw_parts(c.data, c.len as usize).to_vec(),
            )
        })
        .collect();
    SENT.with(|s| *s.borrow_mut() = Some(fields));
}

const HTLC: *mut c_void = std::ptr::dangling_mut::<c_void>();

/// Which fetch a case makes.
#[derive(Debug, Clone, Copy)]
enum Fetch {
    Initial,
    Older { cid: u32, before: u64 },
    Before { before: u64, limit: u16 },
}

#[test]
fn each_fetch_asks_for_what_it_should() {
    let channel = |cid: u32| (tag::CHANNEL_ID, cid.to_be_bytes().to_vec());
    let limit = |n: u16| (tag::HISTORY_LIMIT, n.to_be_bytes().to_vec());
    let before = |id: u64| (tag::HISTORY_BEFORE, id.to_be_bytes().to_vec());
    let older = Fetch::Older {
        cid: 3,
        before: 1000,
    };
    // (cap, newest line seen, which fetch, what goes out)
    let cases = [
        (false, 0, Fetch::Initial, None),
        (true, 0, Fetch::Initial, Some(vec![channel(0), limit(50)])),
        (
            true,
            5000,
            Fetch::Initial,
            Some(vec![
                channel(0),
                (tag::HISTORY_AFTER, 5000u64.to_be_bytes().to_vec()),
            ]),
        ),
        (
            true,
            5000,
            older,
            Some(vec![channel(3), before(1000), limit(50)]),
        ),
        (false, 0, older, None),
        (
            true,
            5000,
            Fetch::Before {
                before: 1000,
                limit: 200,
            },
            Some(vec![channel(0), before(1000), limit(200)]),
        ),
        (
            true,
            0,
            Fetch::Before {
                before: 0,
                limit: 200,
            },
            Some(vec![channel(0), limit(200)]),
        ),
        (
            false,
            0,
            Fetch::Before {
                before: 1000,
                limit: 200,
            },
            None,
        ),
    ];
    for (cap, newest, fetch, want) in cases {
        CAPS.with(|c| c.set(if cap { HTLC_CAP_CHAT_HISTORY } else { 0 }));
        NEWEST.with(|c| c.set(newest));
        SENT.with(|s| *s.borrow_mut() = None);
        crate::send::expected::take();
        crate::recv::chat::history_forget(HTLC);
        let sent = unsafe {
            match fetch {
                Fetch::Initial => hx_chat_history_fetch_initial(HTLC) != GFALSE,
                Fetch::Older { cid, before } => {
                    hx_chat_history_fetch_older(HTLC, cid, before) != GFALSE
                }
                Fetch::Before { before, limit } => fetch_before(HTLC, 0, before, limit),
            }
        };
        let what = format!("cap {cap}, newest {newest}, {fetch:?}");
        assert_eq!(sent, want.is_some(), "{what}");
        assert_eq!(SENT.with(|s| s.borrow_mut().take()), want, "{what}");
        let (cid, first) = match fetch {
            Fetch::Initial => (0, true),
            Fetch::Older { cid, .. } => (cid, false),
            Fetch::Before { before, .. } => (0, before == 0),
        };
        let expected = crate::send::expected::take();
        if sent {
            assert_eq!(
                expected,
                [(1, hxsession::Expect::ChatHistory { cid })],
                "{what}"
            );
            // The newest page is the chat's first since the login, which
            // live lines wait for.
            assert_eq!(
                crate::recv::chat::first_page_pending(HTLC, cid),
                first,
                "{what}"
            );
        } else {
            assert!(expected.is_empty(), "{what}");
        }
    }
}

#[test]
fn a_null_connection_sends_nothing() {
    CAPS.with(|c| c.set(HTLC_CAP_CHAT_HISTORY));
    SENT.with(|s| *s.borrow_mut() = None);
    unsafe {
        assert_eq!(hx_chat_history_fetch_initial(std::ptr::null_mut()), GFALSE);
        assert_eq!(
            hx_chat_history_fetch_older(std::ptr::null_mut(), 0, 1),
            GFALSE
        );
        assert!(!fetch_before(std::ptr::null_mut(), 0, 1, 200));
    }
    assert!(SENT.with(|s| s.borrow().is_none()));
}

/// Lines by id go out as the draft has them only where the server agreed
/// to chat history and replies, each expected as a history reply and
/// recorded as quotes, so its answer is no page.
#[test]
fn quotes_are_asked_for_by_id_only_with_history_and_replies() {
    use hxsession::cap::REPLIES;
    let ids: Vec<u64> = (1..=hxproto::replies::MAX_IDS as u64).collect();
    let most = std::iter::once((tag::CHANNEL_ID, vec![0; 4]))
        .chain(
            ids.iter()
                .map(|id| (tag::LINE_ID, id.to_be_bytes().to_vec())),
        )
        .collect();
    let past: Vec<u64> = (1..=ids.len() as u64 + 1).collect();
    let both = HTLC_CAP_CHAT_HISTORY | REPLIES;
    for (case, caps, ids, want) in [
        ("both", both, &ids[..], Some(most)),
        ("no replies", HTLC_CAP_CHAT_HISTORY, &ids[..], None),
        ("no history", REPLIES, &ids[..], None),
        ("none", both, &[][..], None),
        ("past the most", both, &past[..], None),
    ] {
        CAPS.with(|c| c.set(caps));
        SENT.with(|s| *s.borrow_mut() = None);
        crate::send::expected::take();
        crate::recv::chat::history_forget(HTLC);
        let sent = unsafe { quotes(HTLC, 0, ids) };
        assert_eq!(sent, want.is_some(), "{case}");
        assert_eq!(SENT.with(|s| s.borrow_mut().take()), want, "{case}");
        let expected = crate::send::expected::take();
        if !sent {
            assert!(expected.is_empty(), "{case}");
            continue;
        }
        assert_eq!(
            expected,
            [(1, hxsession::Expect::ChatHistory { cid: 0 })],
            "{case}"
        );
        test_env::reset();
        unsafe { crate::recv::chat::history(HTLC, 1, 0, &[], false) };
        let told = test_env::emitted();
        assert!(
            matches!(&told[..], [Emitted::Quotes { asked, .. }] if asked == ids),
            "{case}: {told:?}"
        );
    }
}
