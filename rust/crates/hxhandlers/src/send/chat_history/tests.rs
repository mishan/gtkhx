//! Send-path unit tests for `hx_get_chat_history`. The native
//! `hotline_proto::build::build_get_chat_history_chunks` runs for real; the cap
//! check and the write primitive are stubbed here (recording what the wrapper
//! handed to `hlwrite_chunks`), so the cargo-test build needs no gtkhx-core
//! accessor / network.c.

use super::*;
use std::cell::{Cell, RefCell};

use hotline_proto::messages::tag;

/// One captured `hlwrite_chunks` call: the wire opcode + each chunk's
/// (tag, copied data bytes).
struct Sent {
    ty: u32,
    chunks: Vec<(u16, Vec<u8>)>,
}

thread_local! {
    static CAP: Cell<bool> = const { Cell::new(false) };
    static LAST_SEND: RefCell<Option<Sent>> = const { RefCell::new(None) };
}

// ---- stubs (the module imports these under cfg(test)) ----------------------

pub(crate) unsafe fn hx_conn_has_cap(_htlc: *const c_void, _cap: u64) -> gboolean {
    if CAP.with(|c| c.get()) {
        GTRUE
    } else {
        GFALSE
    }
}

pub(crate) unsafe fn hlwrite_chunks(
    _htlc: *mut c_void,
    ty: u32,
    _flag: u32,
    chunks: *const HxChunk,
    hc: c_int,
) {
    let mut out = Vec::new();
    for i in 0..hc as usize {
        let c = &*chunks.add(i);
        let data = if c.data.is_null() || c.len == 0 {
            Vec::new()
        } else {
            std::slice::from_raw_parts(c.data, c.len as usize).to_vec()
        };
        out.push((c.tag, data));
    }
    LAST_SEND.with(|s| *s.borrow_mut() = Some(Sent { ty, chunks: out }));
}

// ---- helpers ---------------------------------------------------------------

/// A non-NULL sentinel htlc (the stubs never dereference it).
const HTLC: *mut c_void = std::ptr::dangling_mut::<c_void>();

fn reset(cap: bool) {
    CAP.with(|c| c.set(cap));
    LAST_SEND.with(|s| *s.borrow_mut() = None);
}

fn last() -> Option<Sent> {
    LAST_SEND.with(|s| s.borrow_mut().take())
}

fn chunk(s: &Sent, t: u16) -> Option<&Vec<u8>> {
    s.chunks.iter().find(|(tag, _)| *tag == t).map(|(_, d)| d)
}

// ---- tests -----------------------------------------------------------------

#[test]
fn skipped_without_cap() {
    reset(/*cap=*/ false);
    let sent = unsafe { hx_get_chat_history(HTLC, 0, 0, 0, 0) };
    assert_eq!(sent, GFALSE);
    assert!(last().is_none()); // nothing written
}

#[test]
fn null_htlc_is_false() {
    reset(/*cap=*/ true);
    let sent = unsafe { hx_get_chat_history(std::ptr::null_mut(), 0, 0, 0, 0) };
    assert_eq!(sent, GFALSE);
    assert!(last().is_none());
}

#[test]
fn bare_request_is_channel_only() {
    reset(/*cap=*/ true);
    assert_eq!(unsafe { hx_get_chat_history(HTLC, 0, 0, 0, 0) }, GTRUE);
    let s = last().expect("sent");
    assert_eq!(s.ty, ClientHdr::GetChatHistory as u32); // opcode 700
    assert_eq!(s.chunks.len(), 1);
    assert_eq!(chunk(&s, tag::CHANNEL_ID), Some(&vec![0, 0, 0, 0]));
}

#[test]
fn before_cursor_and_limit() {
    reset(/*cap=*/ true);
    assert_eq!(
        unsafe {
            hx_get_chat_history(
                HTLC, 0, /*before=*/ 1000, /*after=*/ 0, /*limit=*/ 50,
            )
        },
        GTRUE
    );
    let s = last().expect("sent");
    assert_eq!(chunk(&s, tag::CHANNEL_ID), Some(&vec![0, 0, 0, 0]));
    assert_eq!(
        chunk(&s, tag::HISTORY_BEFORE),
        Some(&1000u64.to_be_bytes().to_vec())
    );
    assert_eq!(
        chunk(&s, tag::HISTORY_LIMIT),
        Some(&50u16.to_be_bytes().to_vec())
    );
    assert!(chunk(&s, tag::HISTORY_AFTER).is_none()); // after omitted when 0
}

#[test]
fn after_cursor_only() {
    reset(/*cap=*/ true);
    assert_eq!(
        unsafe {
            hx_get_chat_history(HTLC, 0, 0, /*after=*/ 5000, 0)
        },
        GTRUE
    );
    let s = last().expect("sent");
    assert_eq!(
        chunk(&s, tag::HISTORY_AFTER),
        Some(&5000u64.to_be_bytes().to_vec())
    );
    assert!(chunk(&s, tag::HISTORY_BEFORE).is_none());
    assert!(chunk(&s, tag::HISTORY_LIMIT).is_none());
}

#[test]
fn range_query_has_before_after_limit() {
    reset(/*cap=*/ true);
    assert_eq!(
        unsafe {
            hx_get_chat_history(
                HTLC, 0, /*before=*/ 600, /*after=*/ 200, /*limit=*/ 50,
            )
        },
        GTRUE
    );
    let s = last().expect("sent");
    assert!(chunk(&s, tag::HISTORY_BEFORE).is_some());
    assert!(chunk(&s, tag::HISTORY_AFTER).is_some());
    assert!(chunk(&s, tag::HISTORY_LIMIT).is_some());
    assert_eq!(s.chunks.len(), 4); // channel + before + after + limit
}
