//! Send-path unit tests for the chat wire-out senders. The native
//! `hotline_proto::build` chat builders run for real; the C send-path
//! primitives — the text encoder, the chat-model/caps bridge, the task table,
//! and the write primitive — are stubbed here (recording what the wrapper
//! handed to `hlwrite_chunks` / `task_new`) so the cargo-test build needs no
//! hxtext / chat_send_bridge / tasks.c / network.c / rcv.c.

use super::*;
use std::cell::{Cell, RefCell};

use hotline_proto::build::HxChunk;

// Data-chunk tags the assertions pin (hotline.h / messages::tag).
const TAG_BODY: u16 = 0x0065;
const TAG_UID: u16 = 0x0067;
const TAG_STYLE: u16 = 0x006d;
const TAG_CHAT_ID: u16 = 0x0072;
const TAG_CHAT_SUBJECT: u16 = 0x0073;

/// One captured `hlwrite_chunks` call: the wire opcode + each chunk's
/// (tag, copied data bytes).
struct Sent {
    ty: u32,
    chunks: Vec<(u16, Vec<u8>)>,
}

/// One captured `task_new` call: whether an rcv handler was attached, the
/// opaque ptr arg, and the label.
struct Task {
    has_rcv: bool,
    ptr: usize,
    label: String,
}

thread_local! {
    static CAP: Cell<bool> = const { Cell::new(false) };
    // Chat lookup: None → NULL (unknown); Some(p) → that opaque ptr.
    static LOOKUP: Cell<usize> = const { Cell::new(0) };
    static LAST_SEND: RefCell<Option<Sent>> = const { RefCell::new(None) };
    static LAST_TASK: RefCell<Option<Task>> = const { RefCell::new(None) };
}

// ---- C send-path stubs (lib.rs imports these under cfg(test)) -------

/// UTF-8 mode → verbatim copy of the input (the branch the senders take when
/// CAP_TEXT_ENCODING is set). That's all the tests need — the Mac Roman path
/// is hxtext's own concern, covered by its crate tests. Returns a g_malloc'd,
/// NUL-terminated buffer like the real fn.
pub(crate) unsafe extern "C" fn gtkhx_text_for_wire(
    utf8: *const c_char,
    utf8_len: usize,
    _utf8_mode: glib::ffi::gboolean,
    _is_body: glib::ffi::gboolean,
    out_len: *mut usize,
) -> *mut c_char {
    let buf = glib::ffi::g_malloc(utf8_len + 1) as *mut u8;
    if utf8_len > 0 {
        std::ptr::copy_nonoverlapping(utf8 as *const u8, buf, utf8_len);
    }
    *buf.add(utf8_len) = 0;
    if !out_len.is_null() {
        *out_len = utf8_len;
    }
    buf as *mut c_char
}

pub(crate) unsafe extern "C" fn hx_htlc_text_encoding_cap(
    _htlc: *mut c_void,
) -> glib::ffi::gboolean {
    if CAP.with(|c| c.get()) {
        glib::ffi::GTRUE
    } else {
        glib::ffi::GFALSE
    }
}

pub(crate) unsafe extern "C" fn hx_chat_lookup(_htlc: *mut c_void, _cid: u32) -> *mut c_void {
    LOOKUP.with(|c| c.get()) as *mut c_void
}

pub(crate) unsafe extern "C" fn hx_chat_lookup_or_create(
    _htlc: *mut c_void,
    _cid: u32,
) -> *mut c_void {
    // Emulate "seed if missing": non-NULL sentinel so JOIN always has a chat.
    let v = LOOKUP.with(|c| c.get());
    let v = if v == 0 { 0xC4A7 } else { v };
    v as *mut c_void
}

pub(crate) unsafe extern "C" fn hlwrite_chunks(
    _htlc: *mut c_void,
    ty: u32,
    _flag: u32,
    chunks: *const HxChunk,
    hc: c_int,
) {
    let mut v = Vec::new();
    for i in 0..hc.max(0) as usize {
        let ch = &*chunks.add(i);
        let data = if ch.data.is_null() || ch.len == 0 {
            Vec::new()
        } else {
            std::slice::from_raw_parts(ch.data, ch.len as usize).to_vec()
        };
        v.push((ch.tag, data));
    }
    LAST_SEND.with(|s| *s.borrow_mut() = Some(Sent { ty, chunks: v }));
}

pub(crate) unsafe extern "C" fn task_new(
    _htlc: *mut c_void,
    rcv: Option<RcvTaskFn>,
    ptr: *mut c_void,
    _data: *mut c_void,
    str_: *const c_char,
) -> *mut c_void {
    let label = std::ffi::CStr::from_ptr(str_).to_string_lossy().into_owned();
    LAST_TASK.with(|t| {
        *t.borrow_mut() = Some(Task {
            has_rcv: rcv.is_some(),
            ptr: ptr as usize,
            label,
        })
    });
    std::ptr::null_mut()
}

pub(crate) unsafe extern "C" fn hx_rcv_user_change(
    _h: *mut c_void,
    _frame: *const c_void,
    _frame_len: usize,
    _p: *mut c_void,
    _d: *mut c_void,
) {
}

pub(crate) unsafe extern "C" fn rcv_task_user_list_switch(
    _h: *mut c_void,
    _frame: *const c_void,
    _frame_len: usize,
    _p: *mut c_void,
    _d: *mut c_void,
) {
}

// ---- helpers --------------------------------------------------------

fn reset(cap: bool, lookup: usize) {
    CAP.with(|c| c.set(cap));
    LOOKUP.with(|c| c.set(lookup));
    LAST_SEND.with(|s| *s.borrow_mut() = None);
    LAST_TASK.with(|t| *t.borrow_mut() = None);
}

fn last() -> Option<Sent> {
    LAST_SEND.with(|s| s.borrow_mut().take())
}

fn last_task() -> Option<Task> {
    LAST_TASK.with(|t| t.borrow_mut().take())
}

/// A non-NULL opaque htlc token (the stubs ignore its contents).
fn htlc() -> *mut c_void {
    1usize as *mut c_void
}

fn cstr(s: &str) -> std::ffi::CString {
    std::ffi::CString::new(s).unwrap()
}

// ---- tests ----------------------------------------------------------

#[test]
fn send_chat_public_omits_chat_id() {
    reset(true, 0);
    let body = cstr("hello");
    unsafe { hx_send_chat(htlc(), body.as_ptr(), 0, 0) };
    let s = last().unwrap();
    assert_eq!(s.ty, HTLC_HDR_CHAT);
    // Public chat (cid 0): STYLE + BODY, no CHAT_ID.
    assert_eq!(s.chunks.len(), 2);
    assert_eq!(s.chunks[0].0, TAG_STYLE);
    assert_eq!(s.chunks[0].1, vec![0, 0]); // style 0, u16 BE
    assert_eq!(s.chunks[1].0, TAG_BODY);
    assert_eq!(s.chunks[1].1, b"hello");
    // No reply task for a chat line.
    assert!(last_task().is_none());
}

#[test]
fn send_chat_private_appends_chat_id_and_style() {
    reset(true, 0);
    let body = cstr("hi");
    unsafe { hx_send_chat(htlc(), body.as_ptr(), 0x2a, 0x0001) };
    let s = last().unwrap();
    assert_eq!(s.chunks.len(), 3);
    assert_eq!(s.chunks[0].0, TAG_STYLE);
    assert_eq!(s.chunks[0].1, vec![0, 1]); // style 1
    assert_eq!(s.chunks[1].0, TAG_BODY);
    assert_eq!(s.chunks[2].0, TAG_CHAT_ID);
    assert_eq!(s.chunks[2].1, vec![0, 0, 0, 0x2a]); // cid 42 BE
}

#[test]
fn chat_user_creates_and_registers_task() {
    reset(true, 0);
    unsafe { hx_chat_user(htlc(), 0x0102) };
    let s = last().unwrap();
    assert_eq!(s.ty, HTLC_HDR_CHAT_CREATE);
    assert_eq!(s.chunks.len(), 1);
    assert_eq!(s.chunks[0].0, TAG_UID);
    assert_eq!(s.chunks[0].1, vec![0x01, 0x02]); // uid BE
    let t = last_task().expect("CHAT_CREATE registers a task");
    assert!(t.has_rcv); // hx_rcv_user_change
    assert_eq!(t.label, "chat");
}

#[test]
fn invite_registers_task_without_handler() {
    reset(true, 0);
    unsafe { hx_invite_user(htlc(), 0x0009, 0x0007) };
    let s = last().unwrap();
    assert_eq!(s.ty, HTLC_HDR_CHAT_INVITE);
    assert_eq!(s.chunks.len(), 2);
    assert_eq!(s.chunks[0].0, TAG_CHAT_ID);
    assert_eq!(s.chunks[0].1, vec![0, 0, 0, 7]);
    assert_eq!(s.chunks[1].0, TAG_UID);
    assert_eq!(s.chunks[1].1, vec![0, 9]);
    let t = last_task().expect("CHAT_INVITE registers an ack task");
    // The C original passes a NULL rcv fn — faithfully None here.
    assert!(!t.has_rcv);
    assert_eq!(t.label, "invite");
}

#[test]
fn null_htlc_is_no_op() {
    // A NULL htlc must short-circuit every sender before task_new/hlwrite_chunks
    // (which dereference it on the C side).
    reset(true, 0xABC);
    let body = cstr("hi");
    let subj = cstr("s");
    unsafe {
        hx_send_chat(std::ptr::null_mut(), body.as_ptr(), 0, 0);
        hx_chat_user(std::ptr::null_mut(), 1);
        hx_invite_user(std::ptr::null_mut(), 1, 1);
        hx_chat_join(std::ptr::null_mut(), 1);
        hx_part_chat(std::ptr::null_mut(), 1);
        hx_change_subject(std::ptr::null_mut(), 1, subj.as_ptr());
    }
    assert!(last().is_none());
    assert!(last_task().is_none());
}

#[test]
fn join_passes_chat_ptr_to_task() {
    // Pre-registered chat: the lookup returns this ptr, JOIN carries it.
    reset(true, 0xBEEF);
    unsafe { hx_chat_join(htlc(), 5) };
    let s = last().unwrap();
    assert_eq!(s.ty, HTLC_HDR_CHAT_JOIN);
    assert_eq!(s.chunks.len(), 1);
    assert_eq!(s.chunks[0].0, TAG_CHAT_ID);
    assert_eq!(s.chunks[0].1, vec![0, 0, 0, 5]);
    let t = last_task().expect("CHAT_JOIN registers a user-list-switch task");
    assert!(t.has_rcv); // rcv_task_user_list_switch
    assert_eq!(t.ptr, 0xBEEF); // the chat pointer
    assert_eq!(t.label, "join");
}

#[test]
fn join_seeds_chat_when_unknown() {
    // Unknown cid: lookup_or_create seeds one (non-NULL) and JOIN still fires.
    reset(true, 0);
    unsafe { hx_chat_join(htlc(), 5) };
    let s = last().unwrap();
    assert_eq!(s.ty, HTLC_HDR_CHAT_JOIN);
    let t = last_task().unwrap();
    assert!(t.has_rcv);
    assert_ne!(t.ptr, 0); // seeded, not NULL
}

#[test]
fn part_bails_when_chat_unknown() {
    reset(true, 0); // lookup → NULL
    unsafe { hx_part_chat(htlc(), 5) };
    assert!(last().is_none()); // nothing written
}

#[test]
fn part_sends_chat_id_when_known() {
    reset(true, 0xABC); // lookup → non-NULL
    unsafe { hx_part_chat(htlc(), 0x33) };
    let s = last().unwrap();
    assert_eq!(s.ty, HTLC_HDR_CHAT_PART);
    assert_eq!(s.chunks.len(), 1);
    assert_eq!(s.chunks[0].0, TAG_CHAT_ID);
    assert_eq!(s.chunks[0].1, vec![0, 0, 0, 0x33]);
    assert!(last_task().is_none()); // PART has no task
}

#[test]
fn reject_sends_chat_id_no_task_no_lookup() {
    reset(true, 0); // lookup → NULL, but DECLINE never looks up
    unsafe { hx_reject_chat(htlc(), 0x44) };
    let s = last().unwrap();
    assert_eq!(s.ty, HTLC_HDR_CHAT_DECLINE);
    assert_eq!(s.chunks.len(), 1);
    assert_eq!(s.chunks[0].0, TAG_CHAT_ID);
    assert_eq!(s.chunks[0].1, vec![0, 0, 0, 0x44]);
    assert!(last_task().is_none()); // DECLINE has no task
}

#[test]
fn reject_bails_on_null_htlc() {
    reset(true, 0);
    unsafe { hx_reject_chat(std::ptr::null_mut(), 1) };
    assert!(last().is_none());
}

#[test]
fn change_subject_emits_chat_id_and_subject() {
    reset(true, 0);
    let subj = cstr("Lobby");
    unsafe { hx_change_subject(htlc(), 1, subj.as_ptr()) };
    let s = last().unwrap();
    assert_eq!(s.ty, HTLC_HDR_CHAT_SUBJECT);
    assert_eq!(s.chunks.len(), 2);
    assert_eq!(s.chunks[0].0, TAG_CHAT_ID);
    assert_eq!(s.chunks[0].1, vec![0, 0, 0, 1]);
    assert_eq!(s.chunks[1].0, TAG_CHAT_SUBJECT);
    assert_eq!(s.chunks[1].1, b"Lobby");
    assert!(last_task().is_none());
}

#[test]
fn empty_chat_body_still_emits_style_and_body() {
    reset(true, 0);
    let body = cstr("");
    unsafe { hx_send_chat(htlc(), body.as_ptr(), 0, 0) };
    let s = last().unwrap();
    assert_eq!(s.chunks.len(), 2);
    assert_eq!(s.chunks[1].0, TAG_BODY);
    assert_eq!(s.chunks[1].1.len(), 0);
}
