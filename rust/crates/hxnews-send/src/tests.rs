//! Send-path unit tests for the 1.5 news RPC senders. The native
//! `hotline_proto::build` news builders run for real; the C send-path
//! primitives — `path_to_hldir`, the text encoder, the CAP probe, the task
//! table, the write primitive, the rcv handlers, and the `news_send_bridge`
//! accessors — are stubbed here (recording what the wrapper handed to
//! `hlwrite_chunks` / `task_new`) so the cargo-test build links no C. Mirrors
//! hxchat-send's test shape.

use super::*;
use std::cell::{Cell, RefCell};
use std::ffi::CString;

// Data-chunk tags the assertions pin (hotline.h).
const TAG_CATEGORY: u16 = 0x0142;
const TAG_NEWSPATH: u16 = 0x0145;
const TAG_THREADID: u16 = 0x0146;
const TAG_NEWSTYPE: u16 = 0x0147;
const TAG_NEWSSUBJECT: u16 = 0x0148;
const TAG_NEWSDATA: u16 = 0x014d;
const TAG_PARENTTHREAD: u16 = 0x014e;

struct Sent {
    ty: u32,
    chunks: Vec<(u16, Vec<u8>)>,
}

struct Task {
    has_rcv: bool,
    ptr: usize,
    label: String,
}

thread_local! {
    static CAP: Cell<bool> = const { Cell::new(true) };
    static NODE_PATH: RefCell<CString> = RefCell::new(CString::new("/node").unwrap());
    // When set, the path accessors return NULL (a node cleared during refresh),
    // exercising the senders' NULL-path guard (real path_to_hldir crashes on it).
    static PATH_NULL: Cell<bool> = const { Cell::new(false) };
    static LAST_SEND: RefCell<Option<Sent>> = const { RefCell::new(None) };
    static LAST_TASK: RefCell<Option<Task>> = const { RefCell::new(None) };
}

// ---- C send-path stubs (lib.rs imports these under cfg(test)) -------

/// Treat the path C-string as the wire NEWSPATH bytes verbatim (the real
/// `path_to_hldir` re-encodes "/a/b" into HL dir chunks; the tests only care
/// that the sender round-trips the bytes into the NEWSPATH chunk).
pub(crate) unsafe extern "C" fn path_to_hldir(
    path: *const c_char,
    hldirlen: *mut u16,
    _is_file: c_int,
) -> *mut u8 {
    let bytes = if path.is_null() {
        &[][..]
    } else {
        CStr::from_ptr(path).to_bytes()
    };
    let n = bytes.len();
    let buf = glib::ffi::g_malloc(n.max(1)) as *mut u8;
    if n > 0 {
        std::ptr::copy_nonoverlapping(bytes.as_ptr(), buf, n);
    }
    if !hldirlen.is_null() {
        *hldirlen = n as u16;
    }
    buf
}

/// UTF-8 mode → verbatim copy (the Mac Roman path is hxtext's own concern).
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
    let label = CStr::from_ptr(str_).to_string_lossy().into_owned();
    LAST_TASK.with(|t| {
        *t.borrow_mut() = Some(Task {
            has_rcv: rcv.is_some(),
            ptr: ptr as usize,
            label,
        })
    });
    std::ptr::null_mut()
}

pub(crate) unsafe extern "C" fn rcv_task_news_file(_h: *mut c_void, _f: *const c_void, _fl: usize, _p: *mut c_void, _d: *mut c_void) {}
pub(crate) unsafe extern "C" fn rcv_task_news_post(_h: *mut c_void, _f: *const c_void, _fl: usize, _p: *mut c_void, _d: *mut c_void) {}
pub(crate) unsafe extern "C" fn rcv_task_newscat_list(_h: *mut c_void, _f: *const c_void, _fl: usize, _p: *mut c_void, _d: *mut c_void) {}
pub(crate) unsafe extern "C" fn rcv_task_newsfolder_list(_h: *mut c_void, _f: *const c_void, _fl: usize, _p: *mut c_void, _d: *mut c_void) {}

pub(crate) unsafe extern "C" fn gnews_catalog_path(_g: *mut c_void) -> *const c_char {
    if PATH_NULL.with(|c| c.get()) {
        return std::ptr::null();
    }
    NODE_PATH.with(|p| p.borrow().as_ptr())
}
pub(crate) unsafe extern "C" fn gnews_folder_path(_g: *mut c_void) -> *const c_char {
    if PATH_NULL.with(|c| c.get()) {
        return std::ptr::null();
    }
    NODE_PATH.with(|p| p.borrow().as_ptr())
}

// ---- helpers --------------------------------------------------------

fn reset() {
    CAP.with(|c| c.set(true));
    PATH_NULL.with(|c| c.set(false));
    LAST_SEND.with(|s| *s.borrow_mut() = None);
    LAST_TASK.with(|t| *t.borrow_mut() = None);
}

fn last() -> Option<Sent> {
    LAST_SEND.with(|s| s.borrow_mut().take())
}
fn last_task() -> Option<Task> {
    LAST_TASK.with(|t| t.borrow_mut().take())
}
fn htlc() -> *mut c_void {
    1usize as *mut c_void
}
fn tok() -> *mut c_void {
    0xABCDusize as *mut c_void
}
fn cstr(s: &str) -> CString {
    CString::new(s).unwrap()
}

// ---- tests ----------------------------------------------------------

#[test]
fn fldr_list_sends_path_and_registers_task() {
    reset();
    NODE_PATH.with(|p| *p.borrow_mut() = cstr("/f"));
    unsafe { hx_news15_fldr_list(htlc(), tok()) };

    let s = last().unwrap();
    assert_eq!(s.ty, HTLC_HDR_NEWSDIRLIST);
    assert_eq!(s.chunks.len(), 1);
    assert_eq!(s.chunks[0].0, TAG_NEWSPATH);
    assert_eq!(s.chunks[0].1, b"/f");
    let t = last_task().expect("DIRLIST registers a folder-list task");
    assert!(t.has_rcv);
    assert_eq!(t.ptr, tok() as usize);
    assert_eq!(t.label, "news_folder");
}

#[test]
fn cat_list_sends_path_and_registers_task() {
    reset();
    NODE_PATH.with(|p| *p.borrow_mut() = cstr("/c"));
    unsafe { hx_news15_cat_list(htlc(), tok()) };

    let s = last().unwrap();
    assert_eq!(s.ty, HTLC_HDR_NEWSCATLIST);
    assert_eq!(s.chunks.len(), 1);
    assert_eq!(s.chunks[0], (TAG_NEWSPATH, b"/c".to_vec()));
    let t = last_task().unwrap();
    assert!(t.has_rcv);
    assert_eq!(t.ptr, tok() as usize);
    assert_eq!(t.label, "news_category");
}

#[test]
fn get_post_emits_path_threadid_type() {
    reset();
    const POSTID: u32 = 0x0102_0304; // distinct non-palindrome so byte order shows
    let path = cstr("/cat");
    let mime = cstr("text/plain");
    // target rides into the task ptr (transfer-full); a fake token is fine here
    // since the happy path hands it to task_new rather than unref-ing it.
    unsafe { hx_news15_get_post(htlc(), path.as_ptr(), POSTID, mime.as_ptr(), tok()) };

    let s = last().unwrap();
    assert_eq!(s.ty, HTLC_HDR_GETTHREAD);
    assert_eq!(s.chunks.len(), 3);
    assert_eq!(s.chunks[0], (TAG_NEWSPATH, b"/cat".to_vec()));
    // THREADID is the postid as big-endian bytes.
    assert_eq!(s.chunks[1], (TAG_THREADID, POSTID.to_be_bytes().to_vec()));
    assert_eq!(s.chunks[2], (TAG_NEWSTYPE, b"text/plain".to_vec()));
    let t = last_task().expect("GETTHREAD registers a news_post task");
    assert!(t.has_rcv);
    assert_eq!(t.ptr, tok() as usize);
    assert_eq!(t.label, "news_post");
}

#[test]
fn post_thread_emits_six_chunks_in_order() {
    reset();
    let path = cstr("/cat");
    let subj = cstr("Hello");
    let body = cstr("world");
    unsafe { hx_news15_post_thread(htlc(), path.as_ptr(), subj.as_ptr(), 0x2a, body.as_ptr()) };

    let s = last().unwrap();
    assert_eq!(s.ty, HTLC_HDR_POSTTHREAD);
    assert_eq!(s.chunks.len(), 6);
    assert_eq!(s.chunks[0], (TAG_NEWSPATH, b"/cat".to_vec()));
    assert_eq!(s.chunks[1], (TAG_PARENTTHREAD, vec![0, 0, 0, 0])); // always 0
    assert_eq!(s.chunks[2], (TAG_NEWSTYPE, b"text/plain".to_vec()));
    assert_eq!(s.chunks[3], (TAG_NEWSSUBJECT, b"Hello".to_vec()));
    assert_eq!(s.chunks[4], (TAG_NEWSDATA, b"world".to_vec()));
    assert_eq!(s.chunks[5], (TAG_THREADID, vec![0, 0, 0, 0x2a]));
    let t = last_task().expect("POSTTHREAD registers an ack task");
    assert!(!t.has_rcv); // no reply handler
    assert_eq!(t.label, "news15_post");
}

#[test]
fn delete_thread_emits_path_and_threadid() {
    reset();
    let path = cstr("/cat");
    unsafe { hx_news15_delete_thread(htlc(), path.as_ptr(), 7) };

    let s = last().unwrap();
    assert_eq!(s.ty, HTLC_HDR_DELETETHREAD);
    assert_eq!(s.chunks.len(), 2);
    assert_eq!(s.chunks[0], (TAG_NEWSPATH, b"/cat".to_vec()));
    assert_eq!(s.chunks[1], (TAG_THREADID, vec![0, 0, 0, 7]));
    assert!(!last_task().unwrap().has_rcv);
}

#[test]
fn delete_emits_single_newspath() {
    reset();
    let path = cstr("/cat");
    unsafe { hx_news15_delete(htlc(), path.as_ptr()) };
    let s = last().unwrap();
    assert_eq!(s.ty, HTLC_HDR_DELNEWSDIRCAT);
    assert_eq!(s.chunks, vec![(TAG_NEWSPATH, b"/cat".to_vec())]);
    assert_eq!(last_task().unwrap().label, "news15_rm");
}

#[test]
fn mkcat_emits_path_and_category() {
    reset();
    let path = cstr("/folder");
    let name = cstr("General");
    unsafe { hx_news15_mkcat(htlc(), path.as_ptr(), name.as_ptr()) };
    let s = last().unwrap();
    assert_eq!(s.ty, HTLC_HDR_MAKECATEGORY);
    assert_eq!(s.chunks.len(), 2);
    assert_eq!(s.chunks[0], (TAG_NEWSPATH, b"/folder".to_vec()));
    assert_eq!(s.chunks[1], (TAG_CATEGORY, b"General".to_vec()));
    assert_eq!(last_task().unwrap().label, "news15_mkcat");
}

#[test]
fn mkdir_emits_single_newspath() {
    reset();
    let path = cstr("/folder/new");
    unsafe { hx_news15_mkdir(htlc(), path.as_ptr()) };
    let s = last().unwrap();
    assert_eq!(s.ty, HTLC_HDR_MAKENEWSDIR);
    assert_eq!(s.chunks, vec![(TAG_NEWSPATH, b"/folder/new".to_vec())]);
    assert_eq!(last_task().unwrap().label, "news15_mkdir");
}

#[test]
fn get_news_is_zero_chunk_with_reply_task() {
    reset();
    unsafe { hx_get_news(htlc()) };
    let s = last().unwrap();
    assert_eq!(s.ty, HTLC_HDR_NEWS_GETFILE);
    assert_eq!(s.chunks.len(), 0); // zero-chunk opcode
    let t = last_task().expect("GETFILE registers a news-file task");
    assert!(t.has_rcv);
    assert_eq!(t.label, "news");
}

#[test]
fn post_news_emits_body_no_reply_task() {
    reset();
    let body = cstr("first post");
    unsafe { hx_post_news(htlc(), body.as_ptr(), 10) };
    let s = last().unwrap();
    assert_eq!(s.ty, HTLC_HDR_NEWS_POST);
    // BODY shares tag 0x0065 (hotline.h HTLC_DATA_NEWS_POST reuse).
    assert_eq!(s.chunks, vec![(0x0065u16, b"first post".to_vec())]);
    let t = last_task().expect("NEWS_POST registers an ack task");
    assert!(!t.has_rcv);
    assert_eq!(t.label, "post");
}

#[test]
fn post_news_uses_explicit_len_not_strlen() {
    reset();
    // Only the first 5 bytes are the post; a trailing NUL-terminated tail must
    // not leak in (the sender passes `len`, not strlen).
    let body = cstr("hello");
    unsafe { hx_post_news(htlc(), body.as_ptr(), 3) };
    let s = last().unwrap();
    assert_eq!(s.chunks, vec![(0x0065u16, b"hel".to_vec())]);
}

#[test]
fn null_htlc_is_no_op() {
    reset();
    let path = cstr("/x");
    let name = cstr("n");
    unsafe {
        hx_get_news(std::ptr::null_mut());
        hx_post_news(std::ptr::null_mut(), name.as_ptr(), 1);
        // NULL target so the transfer-full release is a no-op (not an unref).
        hx_news15_get_post(std::ptr::null_mut(), path.as_ptr(), 0, name.as_ptr(),
                           std::ptr::null_mut());
        hx_news15_cat_list(std::ptr::null_mut(), tok());
        hx_news15_fldr_list(std::ptr::null_mut(), tok());
        hx_news15_post_thread(std::ptr::null_mut(), path.as_ptr(), name.as_ptr(), 0, name.as_ptr());
        hx_news15_delete_thread(std::ptr::null_mut(), path.as_ptr(), 1);
        hx_news15_delete(std::ptr::null_mut(), path.as_ptr());
        hx_news15_mkcat(std::ptr::null_mut(), path.as_ptr(), name.as_ptr());
        hx_news15_mkdir(std::ptr::null_mut(), path.as_ptr());
    }
    assert!(last().is_none());
    assert!(last_task().is_none());
}

#[test]
fn null_struct_is_no_op_for_list_senders() {
    reset();
    unsafe {
        hx_news15_cat_list(htlc(), std::ptr::null_mut());
        hx_news15_fldr_list(htlc(), std::ptr::null_mut());
    }
    assert!(last().is_none());
    assert!(last_task().is_none());
}

/// A NULL `path` argument must bail before `path_to_hldir` (not NULL-safe).
#[test]
fn null_path_arg_is_no_op() {
    reset();
    let name = cstr("n");
    unsafe {
        // get_post takes its path directly now; NULL path must bail before
        // path_to_hldir (and release the transfer-full target — NULL here).
        hx_news15_get_post(htlc(), std::ptr::null(), 0, name.as_ptr(), std::ptr::null_mut());
        hx_news15_post_thread(htlc(), std::ptr::null(), name.as_ptr(), 0, name.as_ptr());
        hx_news15_delete_thread(htlc(), std::ptr::null(), 1);
        hx_news15_delete(htlc(), std::ptr::null());
        hx_news15_mkcat(htlc(), std::ptr::null(), name.as_ptr());
        hx_news15_mkdir(htlc(), std::ptr::null());
    }
    assert!(last().is_none());
    assert!(last_task().is_none());
}

/// When the path accessor yields NULL (a node cleared during refresh), the
/// list senders bail before `path_to_hldir` — nothing is sent, no task.
#[test]
fn null_accessor_path_is_no_op() {
    reset();
    PATH_NULL.with(|c| c.set(true));
    unsafe {
        hx_news15_cat_list(htlc(), tok());
        hx_news15_fldr_list(htlc(), tok());
    }
    assert!(last().is_none());
    assert!(last_task().is_none());
}
