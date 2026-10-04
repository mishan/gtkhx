//! What each history fetch puts on the wire. The native builder runs for
//! real; the cap check, the cursor and the write primitive are stubbed,
//! recording what was handed to `hlwrite_chunks`.

use super::*;
use std::cell::{Cell, RefCell};

use hxproto::messages::tag;

/// A request's chunks: (tag, data).
type Fields = Vec<(u16, Vec<u8>)>;

thread_local! {
    static CAP: Cell<bool> = const { Cell::new(false) };
    static NEWEST: Cell<u64> = const { Cell::new(0) };
    static SENT: RefCell<Option<Fields>> = const { RefCell::new(None) };
}

pub(crate) unsafe fn hx_conn_has_cap(_htlc: *const c_void, _cap: u64) -> gboolean {
    if CAP.with(|c| c.get()) {
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

#[test]
fn each_fetch_asks_for_what_it_should() {
    let channel = |cid: u32| (tag::CHANNEL_ID, cid.to_be_bytes().to_vec());
    let limit = |n: u16| (tag::HISTORY_LIMIT, n.to_be_bytes().to_vec());
    // (cap, newest line seen, which fetch, what goes out)
    let cases = [
        (false, 0, None, None),
        (true, 0, None, Some(vec![channel(0), limit(50)])),
        (
            true,
            5000,
            None,
            Some(vec![
                channel(0),
                (tag::HISTORY_AFTER, 5000u64.to_be_bytes().to_vec()),
            ]),
        ),
        (
            true,
            5000,
            Some((3, 1000)),
            Some(vec![
                channel(3),
                (tag::HISTORY_BEFORE, 1000u64.to_be_bytes().to_vec()),
                limit(50),
            ]),
        ),
        (false, 0, Some((3, 1000)), None),
    ];
    for (cap, newest, older, want) in cases {
        CAP.with(|c| c.set(cap));
        NEWEST.with(|c| c.set(newest));
        SENT.with(|s| *s.borrow_mut() = None);
        crate::send::expected::take();
        let sent = unsafe {
            match older {
                None => hx_chat_history_fetch_initial(HTLC),
                Some((cid, before)) => hx_chat_history_fetch_older(HTLC, cid, before),
            }
        };
        let what = format!("cap {cap}, newest {newest}, older {older:?}");
        assert_eq!(sent != GFALSE, want.is_some(), "{what}");
        assert_eq!(SENT.with(|s| s.borrow_mut().take()), want, "{what}");
        let cid = older.map_or(0, |o| o.0);
        let expected = crate::send::expected::take();
        if sent != GFALSE {
            assert_eq!(
                expected,
                [(1, hxsession::Expect::ChatHistory { cid })],
                "{what}"
            );
        } else {
            assert!(expected.is_empty(), "{what}");
        }
    }
}

#[test]
fn a_null_connection_sends_nothing() {
    CAP.with(|c| c.set(true));
    SENT.with(|s| *s.borrow_mut() = None);
    unsafe {
        assert_eq!(hx_chat_history_fetch_initial(std::ptr::null_mut()), GFALSE);
        assert_eq!(
            hx_chat_history_fetch_older(std::ptr::null_mut(), 0, 1),
            GFALSE
        );
    }
    assert!(SENT.with(|s| s.borrow().is_none()));
}
